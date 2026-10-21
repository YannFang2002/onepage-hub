#include "AgentRemoteActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
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

#include "WifiCredentialStore.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr size_t kMaxHostLength = 128;

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
  if (!udp.begin(8767)) {
    return;
  }
  const char hello[] = "{\"type\":\"hello\",\"id\":\"onepage\"}";
  const IPAddress ip = WiFi.localIP();
  const IPAddress mask = WiFi.subnetMask();
  IPAddress broadcast;
  for (int i = 0; i < 4; ++i) {
    broadcast[i] = static_cast<uint8_t>(ip[i] | static_cast<uint8_t>(~mask[i]));
  }
  for (int attempt = 0; attempt < 2; ++attempt) {
    const IPAddress target = attempt == 0 ? broadcast : IPAddress(255, 255, 255, 255);
    udp.beginPacket(target, 8767);
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
  if (!udp.begin(8767)) {
    discovering_ = false;
    loading_ = false;
    error_ = tr(STR_AGENT_NOT_FOUND);
    requestUpdate();
    return false;
  }

  const char hello[] = "{\"type\":\"hello\",\"id\":\"onepage\"}";
  const IPAddress ip = WiFi.localIP();
  const IPAddress mask = WiFi.subnetMask();
  IPAddress broadcast;
  for (int i = 0; i < 4; ++i) {
    broadcast[i] = static_cast<uint8_t>(ip[i] | static_cast<uint8_t>(~mask[i]));
  }
  for (int attempt = 0; attempt < 2; ++attempt) {
    const IPAddress target = attempt == 0 ? broadcast : IPAddress(255, 255, 255, 255);
    udp.beginPacket(target, 8767);
    udp.write(reinterpret_cast<const uint8_t*>(hello), sizeof(hello) - 1);
    udp.endPacket();
  }

  bool found = false;
  const uint32_t startedAt = millis();
  while (millis() - startedAt < 2500) {
    const int size = udp.parsePacket();
    if (size > 0) {
      char buffer[192];
      const int len = udp.read(buffer, sizeof(buffer) - 1);
      if (len > 0) {
        buffer[len] = '\0';
        JsonDocument doc;
        if (!deserializeJson(doc, buffer)) {
          const char* host = doc["host"] | "";
          const int port = doc["port"] | 0;
          if (host[0] != '\0' && port > 0) {
            config_.baseUrl = AgentConfig::normalizeBaseUrl(std::string("http://") + host + ":" +
                                                            std::to_string(port));
            found = true;
            break;
          }
        }
      }
    }
    delay(50);
  }
  udp.stop();

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
    const auto lines = renderer.wrappedText(UI_10_FONT_ID, transcript_.c_str(), contentWidth, maxLines);
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
  const std::string hostValue = configured_ ? config_.baseUrl : tr(STR_AGENT_NOT_CONFIGURED);
  const std::string hostLine = std::string(tr(STR_AGENT_HOST)) + ": " + hostValue;
  const std::string shownHost = renderer.truncatedText(UI_10_FONT_ID, hostLine.c_str(), contentWidth);
  renderer.drawText(UI_10_FONT_ID, left, y, shownHost.c_str());
  y += lineHeight + 6;

  const char* connection;
  if (!configured_) {
    connection = tr(STR_AGENT_NOT_CONFIGURED);
  } else if (wifiConnecting_) {
    connection = tr(STR_AGENT_WIFI_CONNECTING);
  } else if (WiFi.status() != WL_CONNECTED) {
    connection = tr(STR_AGENT_WIFI_REQUIRED);
  } else {
    connection = status_.ok ? tr(STR_AGENT_CONNECTED) : tr(STR_AGENT_DISCONNECTED);
  }
  snprintf(line, sizeof(line), "%s: %s", tr(STR_AGENT_STATUS), connection);
  renderer.drawText(UI_10_FONT_ID, left, y, line);
  y += lineHeight + 6;

  if (configured_ && status_.ok) {
    snprintf(line, sizeof(line), "%s: %s", tr(STR_AGENT_AGENT),
             status_.agent.empty() ? "DSH" : status_.agent.c_str());
    renderer.drawText(UI_10_FONT_ID, left, y, line);
    y += lineHeight + 4;
    if (!status_.repo.empty()) {
      snprintf(line, sizeof(line), "%s: %s", tr(STR_AGENT_REPO), status_.repo.c_str());
      renderer.drawText(UI_10_FONT_ID, left, y, line);
      y += lineHeight + 4;
    }
    if (!status_.state.empty()) {
      snprintf(line, sizeof(line), "%s: %s", tr(STR_AGENT_STATE), status_.state.c_str());
      renderer.drawText(UI_10_FONT_ID, left, y, line);
      y += lineHeight + 4;
    }
  }

  y += 10;
  renderer.drawLine(left, y, width - left, y, 1, true);
  y += 16;
  renderer.drawText(UI_10_FONT_ID, left, y, tr(STR_AGENT_LAST_REPLY));
  y += lineHeight + 4;

  const std::string& reply = !lastReply_.empty() ? lastReply_ : status_.lastReply;
  const int hintTop = renderer.getScreenHeight() - metrics.buttonHintsHeight - 8;
  int maxReplyLines = (hintTop - y - lineHeight) / lineHeight;
  maxReplyLines = std::max(1, std::min(4, maxReplyLines));
  if (reply.empty()) {
    renderer.drawText(SMALL_FONT_ID, left, y, tr(STR_AGENT_NO_REPLY));
    y += lineHeight;
  } else {
    const auto lines = renderer.wrappedText(UI_10_FONT_ID, reply.c_str(), contentWidth, maxReplyLines);
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
      const auto errLines = renderer.wrappedText(SMALL_FONT_ID, error_.c_str(), contentWidth, remaining);
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
