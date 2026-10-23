#include "OnlineUpdateActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_app_desc.h>
#include <esp_ota_ops.h>

#include <string>

#include "components/UITheme.h"
#include "fontIds.h"
#include "network/FirmwareFlasher.h"
#include "network/HttpDownloader.h"

namespace {

// The version baked into the app description is "onepage-hub-1.4.1"; the bridge
// reports plain "1.4.1". Compare the trailing component only.
std::string shortVersion(const char* raw) {
  if (raw == nullptr) {
    return {};
  }
  std::string value(raw);
  const size_t dash = value.rfind('-');
  return dash == std::string::npos ? value : value.substr(dash + 1);
}

}  // namespace

void OnlineUpdateActivity::onEnter() {
  Activity::onEnter();
  config_.load();
  phase_ = Phase::Idle;
  message_ = tr(STR_OTA_IDLE);
  remoteVersion_.clear();
  downloadUrl_.clear();
  percent_ = 0;
  workPending_ = false;
  rebootPending_ = false;
  requestUpdate();
}

void OnlineUpdateActivity::setPhase(const Phase phase, const std::string& message) {
  phase_ = phase;
  message_ = message;
  requestUpdate();
}

bool OnlineUpdateActivity::fetchManifest() {
  if (config_.baseUrl.empty()) {
    setPhase(Phase::Failed, tr(STR_OTA_NO_HOST));
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    setPhase(Phase::Failed, tr(STR_OTA_NO_WIFI));
    return false;
  }

  const std::string manifestUrl = config_.baseUrl + "/api/v1/ota/latest";
  std::string body;
  if (!HttpDownloader::fetchUrl(manifestUrl, body)) {
    setPhase(Phase::Failed, tr(STR_OTA_NETWORK_ERROR));
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, body.c_str()) != DeserializationError::Ok) {
    setPhase(Phase::Failed, tr(STR_OTA_BAD_MANIFEST));
    return false;
  }

  remoteVersion_ = std::string(doc["version"] | "");
  std::string url = std::string(doc["url"] | "");
  if (remoteVersion_.empty() || url.empty()) {
    setPhase(Phase::Failed, tr(STR_OTA_BAD_MANIFEST));
    return false;
  }
  if (url.rfind("http", 0) != 0) {
    url = config_.baseUrl + url;
  }
  downloadUrl_ = url;

  const std::string current = shortVersion(esp_app_get_description()->version);
  LOG_INF("OTA", "current=%s remote=%s url=%s", current.c_str(), remoteVersion_.c_str(),
          downloadUrl_.c_str());
  if (current == remoteVersion_) {
    setPhase(Phase::UpToDate, tr(STR_OTA_UP_TO_DATE));
    return false;
  }
  return true;
}

bool OnlineUpdateActivity::downloadImage() {
  setPhase(Phase::Downloading, tr(STR_OTA_DOWNLOADING));
  if (!Storage.ensureDirectoryExists("/system/.crosspoint/ota")) {
    setPhase(Phase::Failed, tr(STR_OTA_SD_ERROR));
    return false;
  }
  Storage.remove(kStagingPath);

  const auto result = HttpDownloader::downloadToFile(
      downloadUrl_, kStagingPath, [this](size_t done, size_t total) {
        percent_ = total > 0 ? static_cast<int>(done * 100 / total) : 0;
        requestUpdate();
      });
  if (result != HttpDownloader::OK) {
    setPhase(Phase::Failed, tr(STR_OTA_DOWNLOAD_FAILED));
    return false;
  }
  return true;
}

bool OnlineUpdateActivity::flashImage() {
  setPhase(Phase::Verifying, tr(STR_OTA_VERIFYING));
  const esp_partition_t* target = esp_ota_get_next_update_partition(nullptr);
  const size_t slotSize = target != nullptr ? target->size : 0;

  const auto valid = firmware_flash::validateImageFile(kStagingPath, slotSize);
  if (valid != firmware_flash::Result::OK) {
    LOG_ERR("OTA", "validate failed: %s", firmware_flash::resultName(valid));
    setPhase(Phase::Failed, firmware_flash::resultName(valid));
    return false;
  }

  setPhase(Phase::Writing, tr(STR_OTA_WRITING));
  percent_ = 0;
  const auto written = firmware_flash::flashFromSdPath(kStagingPath, nullptr, nullptr, true);
  if (written != firmware_flash::Result::OK) {
    LOG_ERR("OTA", "flash failed: %s", firmware_flash::resultName(written));
    setPhase(Phase::Failed, firmware_flash::resultName(written));
    return false;
  }

  Storage.remove(kStagingPath);
  setPhase(Phase::Done, tr(STR_OTA_DONE));
  rebootPending_ = true;
  return true;
}

void OnlineUpdateActivity::runUpdate() {
  setPhase(Phase::Checking, tr(STR_OTA_CHECKING));
  if (!fetchManifest()) {
    return;
  }
  if (!downloadImage()) {
    return;
  }
  flashImage();
}

void OnlineUpdateActivity::loop() {
  if (rebootPending_) {
    static unsigned long doneAt = 0;
    if (doneAt == 0) {
      doneAt = millis();
      requestUpdateAndWait();
    } else if (millis() - doneAt > 1200) {
      ESP.restart();
    }
    return;
  }

  if (workPending_) {
    workPending_ = false;
    runUpdate();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (phase_ != Phase::Downloading && phase_ != Phase::Writing) {
      workPending_ = true;
      requestUpdate();
    }
  }
}

void OnlineUpdateActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight},
                 tr(STR_ONLINE_UPDATE));

  const int left = metrics.contentSidePadding;
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  int y = metrics.topPadding + metrics.headerHeight + 24;

  renderer.drawText(UI_10_FONT_ID, left, y, message_.c_str());
  y += lineHeight + 10;

  if (phase_ == Phase::Downloading && percent_ > 0) {
    char progress[16];
    snprintf(progress, sizeof(progress), "%d%%", percent_);
    renderer.drawText(UI_10_FONT_ID, left, y, progress);
    y += lineHeight + 6;
  }

  if (!remoteVersion_.empty()) {
    const std::string line = std::string(tr(STR_OTA_VERSION)) + " " + remoteVersion_;
    renderer.drawText(SMALL_FONT_ID, left, y, line.c_str());
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OTA_CHECK), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
