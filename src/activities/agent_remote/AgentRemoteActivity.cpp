#include "AgentRemoteActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Utf8.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "WifiCredentialStore.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr size_t kMaxHostLength = 128;
// Discovery port. The device and the host bridge both speak it, and every
// packet carries protocol "ONEPAGE-HUB" so other UDP 8767 services ignore us.
constexpr uint16_t kDiscoveryPort = 8767;

std::string friendlyError(const std::string& raw) {
  if (raw.empty()) {
    return {};
  }
  if (raw == "not_configured") {
    return tr(STR_AGENT_NOT_CONFIGURED);
  }
  if (raw == "network") {
    return tr(STR_AGENT_NETWORK_ERROR);
  }
  if (raw == "bad_json") {
    return tr(STR_AGENT_BAD_RESPONSE);
  }
  if (raw == "dsh_unavailable" || raw == "device_not_paired") {
    return tr(STR_AGENT_DISCONNECTED);
  }
  return raw;
}

void trimTrailingSpaces(std::string& text) {
  while (!text.empty() && text.back() == ' ') {
    text.pop_back();
  }
}

// Maps the host's agent state onto a word a person can read at a glance, instead
// of echoing the raw protocol value ("idle"/"running"/"error").
const char* agentStateText(const std::string& state) {
  if (state == "running") {
    return tr(STR_AGENT_RUNNING);
  }
  if (state == "idle") {
    return tr(STR_AGENT_IDLE);
  }
  if (state == "error") {
    return tr(STR_AGENT_ERROR);
  }
  return tr(STR_AGENT_UNKNOWN);
}

// The panel cannot render Markdown and a full agent reply never fits on a
// 480x800 e-ink page, so only the first sentence is shown: drop Markdown
// markers, collapse whitespace, then cut at the first sentence terminator.
std::string summarizeReply(const std::string& raw) {
  std::string cleaned;
  cleaned.reserve(raw.size());
  bool lastWasSpace = true;
  for (const char c : raw) {
    if (c == '*' || c == '`' || c == '#' || c == '_' || c == '~') {
      continue;  // Markdown emphasis/code markers
    }
    const bool space = c == '\n' || c == '\r' || c == '\t' || c == ' ';
    if (space) {
      if (!lastWasSpace && !cleaned.empty()) {
        cleaned.push_back(' ');
      }
      lastWasSpace = true;
      continue;
    }
    cleaned.push_back(c);
    lastWasSpace = false;
  }

  const size_t end = cleaned.find_first_of("。！？!?");
  if (end != std::string::npos) {
    cleaned.resize(end + 1);
  }
  return cleaned;
}

// Wraps `text` to `maxWidth` using `fontId`.
//
// GfxRenderer::wrappedText() only breaks on ASCII spaces, which is fine for the
// English UI strings it was written for but useless here: a Chinese transcription
// has no spaces at all, so the whole sentence counted as one "word" and came back
// as a single truncated line -- the text then ran off the right edge of the panel
// instead of wrapping onto the next line.
//
// This version breaks by measured pixel width and accepts the same break
// opportunities a Chinese reader expects: after a space, or between two CJK
// codepoints. Latin words are only split when a single word is wider than the
// line. Returns at most `maxLines` lines, with the last one ellipsised when the
// text does not fit (truncatedText() adds the ellipsis itself).
std::vector<std::string> wrapDisplayText(const GfxRenderer& renderer, const int fontId, const std::string& text,
                                         const int maxWidth, const int maxLines) {
  std::vector<std::string> lines;
  if (maxWidth <= 0 || maxLines <= 0 || text.empty()) {
    return lines;
  }

  // ASR output arrives as one blob and may contain newlines/tabs from the
  // gateway; collapse any whitespace run into a single space first.
  std::string source;
  source.reserve(text.size());
  for (const char c : text) {
    const bool space = c == '\n' || c == '\r' || c == '\t' || c == ' ';
    if (space) {
      if (!source.empty() && source.back() != ' ') {
        source.push_back(' ');
      }
    } else {
      source.push_back(c);
    }
  }
  trimTrailingSpaces(source);

  size_t pos = 0;
  while (pos < source.size() && static_cast<int>(lines.size()) < maxLines) {
    std::string line;
    size_t cursor = pos;
    size_t breakAfterSpace = 0;  // byte offset in `line` just past the last space
    size_t breakAfterCjk = 0;    // byte offset in `line` just past the last CJK char

    while (cursor < source.size()) {
      const unsigned char* start = reinterpret_cast<const unsigned char*>(source.c_str()) + cursor;
      const unsigned char* next = start;
      const uint32_t cp = utf8NextCodepoint(&next);
      const size_t cpLen = static_cast<size_t>(next - start);

      line.append(source, cursor, cpLen);
      if (renderer.getTextWidth(fontId, line.c_str()) > maxWidth) {
        line.resize(line.size() - cpLen);  // this codepoint does not fit
        break;
      }
      cursor += cpLen;
      if (cp == ' ') {
        breakAfterSpace = line.size();
      } else if (utf8IsCjkBreakable(cp)) {
        breakAfterCjk = line.size();
      }
    }

    if (cursor >= source.size()) {
      lines.push_back(line);  // the remainder fit on this line
      break;
    }

    size_t cut = line.size();
    if (breakAfterSpace > 0) {
      cut = breakAfterSpace;  // keep the space itself out of the line
    } else if (breakAfterCjk > 0) {
      cut = breakAfterCjk;
    }
    if (cut == 0) {
      // A single codepoint is wider than the whole line: emit it anyway so the
      // loop always makes progress (truncation handles the visual overflow).
      const unsigned char* start = reinterpret_cast<const unsigned char*>(source.c_str()) + pos;
      const unsigned char* next = start;
      utf8NextCodepoint(&next);
      cut = static_cast<size_t>(next - start);
      lines.push_back(source.substr(pos, cut));
    } else {
      std::string out = line.substr(0, cut);
      trimTrailingSpaces(out);
      lines.push_back(std::move(out));
    }

    size_t advance = pos + cut;
    while (advance < source.size() && source[advance] == ' ') {
      ++advance;
    }
    if (advance <= pos) {
      advance = pos + cut;  // guarantee forward progress
    }
    pos = advance;
  }

  if (pos < source.size() && !lines.empty()) {
    // Ran out of lines with text left: ellipsise the last one.
    lines.back() = renderer.truncatedText(fontId, (lines.back() + " " + source.substr(pos)).c_str(), maxWidth);
  }
  return lines;
}
}  // namespace

void AgentRemoteActivity::onEnter() {
  Activity::onEnter();
  config_.load();
  configured_ = !config_.baseUrl.empty();
  lastReply_.clear();
  error_.clear();
  transcript_.clear();
  recording_ = false;
  transcribing_ = false;
  preview_ = false;
  discovering_ = false;
  discoveryTried_ = false;
  if (configured_) {
    if (ensureNetwork()) {
      announceHost();
      refreshStatus();
    } else {
      requestUpdate();
    }
  } else if (ensureNetwork()) {
    discoverHost();
  } else {
    requestUpdate();
  }
}

void AgentRemoteActivity::onExit() {
  if (recording_ || transcribing_) {
    voice_.stop();
  }
  voice_.release();
  Activity::onExit();
}

bool AgentRemoteActivity::ensureNetwork() {
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnecting_ = false;
    return true;
  }

  if (wifiConnecting_) {
    if (millis() - wifiStartMs_ > 15000) {
      wifiConnecting_ = false;
      error_ = tr(STR_AGENT_WIFI_FAILED);
      requestUpdate();
    }
    return false;
  }

  WIFI_STORE.loadFromFile();
  const auto& credentials = WIFI_STORE.getCredentials();
  if (credentials.empty()) {
    error_ = tr(STR_AGENT_WIFI_REQUIRED);
    requestUpdate();
    return false;
  }

  const WifiCredential* credential = nullptr;
  const std::string& lastSsid = WIFI_STORE.getLastConnectedSsid();
  if (!lastSsid.empty()) {
    credential = WIFI_STORE.findCredential(lastSsid);
  }
  if (credential == nullptr) {
    credential = &credentials.front();
  }

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(100);
  if (credential->password.empty()) {
    WiFi.begin(credential->ssid.c_str());
  } else {
    WiFi.begin(credential->ssid.c_str(), credential->password.c_str());
  }

  wifiConnecting_ = true;
  wifiStartMs_ = millis();
  error_ = tr(STR_AGENT_WIFI_CONNECTING);
  requestUpdate();
  return false;
}

void AgentRemoteActivity::startWifiSetup() {
  wifiConnecting_ = false;
  auto wifi = std::make_unique<WifiSelectionActivity>(renderer, mappedInput, true);
  startActivityForResult(std::move(wifi), [this](const ActivityResult& result) {
    if (!result.isCancelled && std::holds_alternative<WifiResult>(result.data) &&
        std::get<WifiResult>(result.data).connected) {
      error_.clear();
      refreshStatus();
    } else {
      error_ = tr(STR_AGENT_WIFI_FAILED);
      requestUpdate();
    }
  });
}

void AgentRemoteActivity::refreshStatus() {
  if (!configured_) {
    requestUpdate();
    return;
  }
  if (!ensureNetwork()) {
    requestUpdate();
    return;
  }

  loading_ = true;
  error_.clear();
  requestUpdateAndWait();

  announceHost();

  AgentStatus next;
  std::string err;
  if (AgentClient::fetchStatus(config_.baseUrl, next, err)) {
    status_ = next;
    if (!next.lastReply.empty()) {
      lastReply_ = next.lastReply;
    }
  } else {
    status_.ok = false;
    error_ = friendlyError(err);
    if (err == "network" && !discoveryTried_) {
      discoveryTried_ = true;
      loading_ = false;
      discoverHost();
      return;
    }
  }

  loading_ = false;
  requestUpdate();
}

bool AgentRemoteActivity::waitForNetwork() {
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnecting_ = false;
    return true;
  }
  if (!ensureNetwork()) {
    if (!wifiConnecting_) {
      return false;
    }
    const unsigned long startedAt = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startedAt < 10000) {
      delay(100);
    }
    if (WiFi.status() != WL_CONNECTED) {
      wifiConnecting_ = false;
      error_ = tr(STR_AGENT_WIFI_FAILED);
      return false;
    }
    wifiConnecting_ = false;
  }
  return true;
}

void AgentRemoteActivity::announceHost() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }
  WiFiUDP udp;
  if (!udp.begin(kDiscoveryPort)) {
    return;
  }
  const char hello[] = "{\"type\":\"hello\",\"protocol\":\"ONEPAGE-HUB\",\"id\":\"onepage\"}";
  const IPAddress ip = WiFi.localIP();
  const IPAddress mask = WiFi.subnetMask();
  IPAddress broadcast;
  for (int i = 0; i < 4; ++i) {
    broadcast[i] = static_cast<uint8_t>(ip[i] | static_cast<uint8_t>(~mask[i]));
  }
  for (int attempt = 0; attempt < 2; ++attempt) {
    const IPAddress target = attempt == 0 ? broadcast : IPAddress(255, 255, 255, 255);
    udp.beginPacket(target, kDiscoveryPort);
    udp.write(reinterpret_cast<const uint8_t*>(hello), sizeof(hello) - 1);
    udp.endPacket();
  }
  udp.stop();
}

bool AgentRemoteActivity::discoverHost() {
  if (discovering_) {
    return false;
  }
  discovering_ = true;
  discoveryTried_ = true;
  loading_ = true;
  error_ = tr(STR_AGENT_DISCOVERING);
  requestUpdateAndWait();

  WiFiUDP udp;
  if (!udp.begin(kDiscoveryPort)) {
    discovering_ = false;
    loading_ = false;
    error_ = tr(STR_AGENT_NOT_FOUND);
    requestUpdate();
    return false;
  }

  const IPAddress ip = WiFi.localIP();
  const IPAddress mask = WiFi.subnetMask();
  IPAddress broadcast;
  for (int i = 0; i < 4; ++i) {
    broadcast[i] = static_cast<uint8_t>(ip[i] | static_cast<uint8_t>(~mask[i]));
  }

  // Two-way discovery, so neither side ever needs a typed address:
  //   device -> host: we broadcast HELLO, the bridge answers with its address;
  //   host -> device: the bridge broadcasts a scan probe that carries its own
  //                   address, and we answer + adopt it. This path still works
  //                   when our broadcast is filtered but the host's is not.
  // Every packet is tagged protocol "ONEPAGE-HUB" so unrelated UDP 8767
  // listeners on the LAN ignore it.
  const char hello[] = "{\"type\":\"hello\",\"protocol\":\"ONEPAGE-HUB\",\"id\":\"onepage\"}";

  std::string host;
  int port = 0;
  bool found = false;

  constexpr int kRounds = 3;
  constexpr uint32_t kRoundMs = 4000;

  for (int round = 0; round < kRounds && !found; ++round) {
    for (int attempt = 0; attempt < 2; ++attempt) {
      const IPAddress target = attempt == 0 ? broadcast : IPAddress(255, 255, 255, 255);
      udp.beginPacket(target, kDiscoveryPort);
      udp.write(reinterpret_cast<const uint8_t*>(hello), sizeof(hello) - 1);
      udp.endPacket();
    }

    const uint32_t roundStartedAt = millis();
    while (!found && millis() - roundStartedAt < kRoundMs) {
      const int size = udp.parsePacket();
      if (size <= 0) {
        delay(20);  // keep the socket serviced without busy-waiting
        continue;
      }

      char buffer[192];
      const int len = udp.read(buffer, sizeof(buffer) - 1);
      if (len <= 0) {
        continue;
      }
      buffer[len] = '\0';

      JsonDocument doc;
      if (deserializeJson(doc, buffer)) {
        continue;
      }

      const char* kind = doc["type"] | "";
      if (strcmp(kind, "onepage-scan") == 0) {
        // The host is looking for us. Answer so it can record our address, then
        // fall through and adopt the host address carried in the same probe.
        JsonDocument reply;
        reply["type"] = "onepage-hub";
        reply["protocol"] = "ONEPAGE-HUB";
        reply["name"] = "ONEPAGE-HUB";
        reply["port"] = kDiscoveryPort;
        char out[96];
        const size_t outLen = serializeJson(reply, out, sizeof(out));
        udp.beginPacket(udp.remoteIP(), udp.remotePort());
        udp.write(reinterpret_cast<const uint8_t*>(out), outLen);
        udp.endPacket();
        LOG_INF("AGENT", "host scan probe seen, replied");
      } else if (strcmp(kind, "host") != 0) {
        continue;  // not our protocol
      }

      const char* candidateHost = doc["host"] | "";
      const int candidatePort = doc["port"] | 0;
      if (candidateHost[0] != '\0' && candidatePort > 0) {
        host = candidateHost;
        port = candidatePort;
        found = true;
      }
    }
  }
  udp.stop();

  if (found) {
    config_.baseUrl = AgentConfig::normalizeBaseUrl(std::string("http://") + host + ":" + std::to_string(port));
  }

  discovering_ = false;
  loading_ = false;

  if (!found) {
    error_ = tr(STR_AGENT_NOT_FOUND);
    requestUpdate();
    return false;
  }

  if (!config_.save()) {
    error_ = tr(STR_AGENT_SAVE_FAILED);
    requestUpdate();
    return false;
  }

  configured_ = true;
  status_ = {};
  lastReply_.clear();
  error_.clear();
  refreshStatus();
  return true;
}

void AgentRemoteActivity::startVoice() {
  if (!configured_) {
    startHostInput();
    return;
  }

  // PDM/I2S needs internal DMA memory that WiFi occupies on this board. Drop the
  // radio for the capture, then reconnect before the upload.
  if (WiFi.getMode() != WIFI_OFF) {
    WiFi.disconnect(false, false);
    WiFi.mode(WIFI_OFF);
    delay(200);
  }
  wifiConnecting_ = false;

  LOG_INF("VOICE", "mic ensure: internal=%u psram=%u", static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
  if (!voice_.ensure()) {
    error_ = tr(STR_AGENT_MIC_FAIL);
    ensureNetwork();
    requestUpdate();
    return;
  }
  if (!voice_.start()) {
    error_ = tr(STR_AGENT_MIC_FAIL);
    voice_.release();
    ensureNetwork();
    requestUpdate();
    return;
  }

  recording_ = true;
  preview_ = false;
  transcript_.clear();
  error_.clear();
  recordStartMs_ = millis();
  lastRecordUiMs_ = recordStartMs_;
  requestUpdate();
}

void AgentRemoteActivity::finishVoice() {
  recording_ = false;
  transcribing_ = true;
  error_.clear();
  requestUpdateAndWait();

  std::string wav;
  const bool captured = voice_.buildWav(wav);
  const bool captureFailed = voice_.failed();
  // Free the I2S channel and its internal DMA memory before bringing WiFi back.
  voice_.release();

  if (!captured) {
    error_ = captureFailed ? tr(STR_AGENT_MIC_FAIL) : tr(STR_AGENT_RECORD_TOO_SHORT);
    ensureNetwork();
    transcribing_ = false;
    requestUpdate();
    return;
  }

  if (!waitForNetwork()) {
    transcribing_ = false;
    requestUpdate();
    return;
  }

  std::string text;
  std::string err;
  if (AgentClient::transcribe(config_.baseUrl, wav, text, err)) {
    transcript_ = text;
    preview_ = true;
  } else {
    error_ = friendlyError(err);
  }

  transcribing_ = false;
  requestUpdate();
}

void AgentRemoteActivity::cancelVoice() {
  voice_.release();
  recording_ = false;
  preview_ = false;
  transcript_.clear();
  error_.clear();
  if (WiFi.getMode() == WIFI_OFF) {
    ensureNetwork();
  }
  requestUpdate();
}

void AgentRemoteActivity::startHostInput() {
  auto keyboard = std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_AGENT_HOST), config_.baseUrl,
                                                          kMaxHostLength, InputType::Url);
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    if (result.isCancelled) {
      requestUpdate();
      return;
    }
    applyHost(std::get<KeyboardResult>(result.data).text);
  });
}

void AgentRemoteActivity::applyHost(const std::string& input) {
  config_.baseUrl = AgentConfig::normalizeBaseUrl(input);
  if (!config_.save()) {
    error_ = tr(STR_AGENT_SAVE_FAILED);
    configured_ = false;
    requestUpdate();
    return;
  }
  configured_ = !config_.baseUrl.empty();
  status_ = {};
  lastReply_.clear();
  error_.clear();
  discoveryTried_ = false;
  if (configured_) {
    refreshStatus();
  } else {
    requestUpdate();
  }
}

bool AgentRemoteActivity::sendMessage(const std::string& text) {
  if (!waitForNetwork()) {
    requestUpdate();
    return false;
  }

  sending_ = true;
  error_.clear();
  requestUpdateAndWait();

  std::string reply;
  std::string err;
  const bool ok = AgentClient::sendText(config_.baseUrl, text, reply, err);
  if (ok) {
    lastReply_ = reply.empty() ? tr(STR_AGENT_SENT) : reply;
    status_.lastReply = lastReply_;
    status_.ok = true;
    preview_ = false;
    transcript_.clear();
  } else {
    error_ = friendlyError(err);
  }

  sending_ = false;
  requestUpdate();
  return ok;
}

void AgentRemoteActivity::loop() {
  if (wifiConnecting_) {
    if (WiFi.status() == WL_CONNECTED) {
      wifiConnecting_ = false;
      error_.clear();
      discoveryTried_ = false;
      if (!configured_) {
        discoverHost();
      } else {
        announceHost();
        refreshStatus();
      }
      return;
    }
    if (millis() - wifiStartMs_ > 15000) {
      wifiConnecting_ = false;
      error_ = tr(STR_AGENT_WIFI_FAILED);
      requestUpdate();
      return;
    }
    return;
  }

  // Liveness: while this app is open the host keeps a "device online" timer that
  // expires after 60 s. A sleeping or closed device simply stops pinging, which
  // is what makes the host stop showing a stale address as connected.
  if (configured_ && !recording_ && !transcribing_ && WiFi.status() == WL_CONNECTED &&
      millis() - lastHeartbeatMs_ >= 20000) {
    lastHeartbeatMs_ = millis();
    AgentClient::heartbeat(config_.baseUrl);
  }

  if (preview_) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      sendMessage(transcript_);
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      cancelVoice();
    }
    return;
  }

  if (recording_) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      cancelVoice();
      return;
    }
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm) ||
        millis() - recordStartMs_ >= 15000) {
      finishVoice();
      return;
    }
    if (millis() - lastRecordUiMs_ >= 5000) {
      lastRecordUiMs_ = millis();
      requestUpdate();
    }
    return;
  }

  if (transcribing_ || loading_ || sending_) {
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (!configured_) {
      discoverHost();
      return;
    }
    if (WiFi.status() != WL_CONNECTED) {
      startWifiSetup();
      return;
    }
    startVoice();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    startHostInput();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    discoveryTried_ = false;
    if (!configured_) {
      discoverHost();
    } else {
      refreshStatus();
    }
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onGoDesktop();
  }
}

void AgentRemoteActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_AGENT_REMOTE));

  const int left = metrics.contentSidePadding;
  const int contentWidth = width - left * 2;
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  int y = metrics.topPadding + metrics.headerHeight + 24;

  if (recording_) {
    renderer.drawText(UI_10_FONT_ID, left, y, tr(STR_AGENT_RECORDING));
    y += 34;
    const unsigned seconds = static_cast<unsigned>((millis() - recordStartMs_) / 1000);
    char duration[32];
    snprintf(duration, sizeof(duration), "%u / 15 s", seconds);
    renderer.drawText(UI_10_FONT_ID, left, y, duration);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_AGENT_STOP), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (transcribing_) {
    renderer.drawText(UI_10_FONT_ID, left, y, tr(STR_AGENT_TRANSCRIBING));
    renderer.displayBuffer();
    return;
  }

  if (sending_) {
    renderer.drawText(UI_10_FONT_ID, left, y, tr(STR_AGENT_SENDING));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (preview_) {
    renderer.drawText(UI_10_FONT_ID, left, y, tr(STR_AGENT_TRANSCRIPT));
    y += lineHeight + 6;
    const int hintTop = renderer.getScreenHeight() - metrics.buttonHintsHeight - 8;
    const int reserve = error_.empty() ? 0 : 1;
    const int maxLines = std::max(1, std::min(8, (hintTop - y) / lineHeight - reserve));
    const auto lines = wrapDisplayText(renderer, UI_10_FONT_ID, transcript_, contentWidth, maxLines);
    for (const auto& text : lines) {
      renderer.drawText(UI_10_FONT_ID, left, y, text.c_str());
      y += lineHeight;
    }
    if (!error_.empty() && y + lineHeight <= hintTop) {
      const std::string shown = renderer.truncatedText(SMALL_FONT_ID, error_.c_str(), contentWidth);
      renderer.drawText(SMALL_FONT_ID, left, y + 4, shown.c_str());
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_AGENT_SEND), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  char line[192];
  const bool wifiOk = WiFi.status() == WL_CONNECTED;
  const int hintTop = renderer.getScreenHeight() - metrics.buttonHintsHeight - 8;

  // Section 1 — link. WiFi and the host link fail independently (wrong SSID vs.
  // host asleep / bridge down), so they get separate rows; the old single
  // "Status" line blended them with the agent's own state.
  renderer.drawText(SMALL_FONT_ID, left, y, tr(STR_AGENT_LINK));
  y += lineHeight + 2;

  snprintf(line, sizeof(line), "  WiFi   %s", wifiOk ? tr(STR_AGENT_CONNECTED) : tr(STR_AGENT_WIFI_REQUIRED));
  renderer.drawText(UI_10_FONT_ID, left, y, line);
  y += lineHeight;

  const char* hostState;
  if (!configured_) {
    hostState = tr(STR_AGENT_NOT_CONFIGURED);
  } else if (wifiConnecting_) {
    hostState = tr(STR_AGENT_WIFI_CONNECTING);
  } else if (!wifiOk) {
    hostState = tr(STR_AGENT_WIFI_REQUIRED);
  } else {
    hostState = status_.ok ? tr(STR_AGENT_CONNECTED) : tr(STR_AGENT_DISCONNECTED);
  }

  std::string hostRow = std::string("  ") + tr(STR_AGENT_HOST) + "   " + hostState;
  if (configured_ && status_.ok) {
    const std::string& url = config_.baseUrl;
    const size_t scheme = url.find("://");
    hostRow += " · ";
    hostRow += (scheme == std::string::npos) ? url : url.substr(scheme + 3);
  }
  {
    const std::string shown = renderer.truncatedText(UI_10_FONT_ID, hostRow.c_str(), contentWidth);
    renderer.drawText(UI_10_FONT_ID, left, y, shown.c_str());
  }
  y += lineHeight + 10;

  // Section 2 — what the agent itself is doing (independent of the link).
  renderer.drawLine(left, y, width - left, y, 1, true);
  y += 12;
  renderer.drawText(SMALL_FONT_ID, left, y, tr(STR_AGENT_AGENT));
  y += lineHeight + 2;

  std::string who = std::string("  ") + (status_.agent.empty() ? "DSH" : status_.agent.c_str());
  if (status_.ok && !status_.repo.empty()) {
    who += " · " + status_.repo;
  }
  {
    const std::string shown = renderer.truncatedText(UI_10_FONT_ID, who.c_str(), contentWidth);
    renderer.drawText(UI_10_FONT_ID, left, y, shown.c_str());
  }
  y += lineHeight;

  snprintf(line, sizeof(line), "  %s   %s", tr(STR_AGENT_STATE),
           status_.ok ? agentStateText(status_.state) : tr(STR_AGENT_UNKNOWN));
  renderer.drawText(UI_10_FONT_ID, left, y, line);
  y += lineHeight + 10;

  // Section 3 — the reply. Markdown cannot be rendered here and a full answer
  // never fits, so the first sentence is the contract: the host trims it, and
  // summarizeReply() enforces it again on the device.
  renderer.drawLine(left, y, width - left, y, 1, true);
  y += 12;
  renderer.drawText(SMALL_FONT_ID, left, y, tr(STR_AGENT_LAST_REPLY));
  y += lineHeight + 4;

  const std::string rawReply = !lastReply_.empty() ? lastReply_ : status_.lastReply;
  const std::string reply = summarizeReply(rawReply);
  int maxReplyLines = (hintTop - y - lineHeight) / lineHeight;
  maxReplyLines = std::max(1, std::min(3, maxReplyLines));
  if (reply.empty()) {
    renderer.drawText(SMALL_FONT_ID, left, y, tr(STR_AGENT_NO_REPLY));
    y += lineHeight;
  } else {
    const auto lines = wrapDisplayText(renderer, UI_10_FONT_ID, reply, contentWidth, maxReplyLines);
    for (const auto& text : lines) {
      renderer.drawText(UI_10_FONT_ID, left, y, text.c_str());
      y += lineHeight;
    }
  }

  y += 6;
  if (y + lineHeight <= hintTop) {
    if (sending_) {
      renderer.drawText(SMALL_FONT_ID, left, y, tr(STR_AGENT_SENDING));
    } else if (loading_) {
      renderer.drawText(SMALL_FONT_ID, left, y, tr(STR_AGENT_LOADING));
    } else if (!error_.empty()) {
      const int remaining = std::max(1, std::min(2, (hintTop - y) / lineHeight));
      const auto errLines = wrapDisplayText(renderer, SMALL_FONT_ID, error_, contentWidth, remaining);
      int ey = y;
      for (const auto& text : errLines) {
        renderer.drawText(SMALL_FONT_ID, left, ey, text.c_str());
        ey += lineHeight;
      }
    }
  }

  const char* confirmLabel;
  if (!configured_) {
    confirmLabel = tr(STR_AGENT_DISCOVER);
  } else if (WiFi.status() == WL_CONNECTED) {
    confirmLabel = tr(STR_AGENT_HOLD_TO_TALK);
  } else {
    confirmLabel = tr(STR_AGENT_WIFI_SETUP);
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_AGENT_SETTINGS),
                                            tr(STR_AGENT_REFRESH));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
