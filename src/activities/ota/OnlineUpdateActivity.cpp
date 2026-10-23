#include "OnlineUpdateActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_task_wdt.h>

#include <variant>

#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/CrossPointWebServer.h"

OnlineUpdateActivity::~OnlineUpdateActivity() { stopServer(); }

void OnlineUpdateActivity::onEnter() {
  Activity::onEnter();
  state_ = State::WifiWait;
  rebootAt_ = 0;
  requestUpdate();

  if (WiFi.status() == WL_CONNECTED) {
    startServer();
    return;
  }
  // No network yet: ask the user to pick one instead of dead-ending on a
  // "connect WiFi" message.
  auto wifi = std::make_unique<WifiSelectionActivity>(renderer, mappedInput, true);
  startActivityForResult(std::move(wifi), [this](const ActivityResult& result) {
    if (!result.isCancelled && std::holds_alternative<WifiResult>(result.data) &&
        std::get<WifiResult>(result.data).connected) {
      startServer();
    } else {
      state_ = State::Error;
      requestUpdate();
    }
  });
}

void OnlineUpdateActivity::onExit() {
  stopServer();
  Activity::onExit();
}

void OnlineUpdateActivity::startServer() {
  state_ = State::ServerStarting;
  requestUpdate();
  webServer_.reset(new CrossPointWebServer());
  webServer_->begin();
  state_ = webServer_->isRunning() ? State::ServerRunning : State::Error;
  requestUpdate();
}

void OnlineUpdateActivity::stopServer() {
  if (webServer_) {
    webServer_->stop();
    webServer_.reset();
  }
}

std::string OnlineUpdateActivity::endpointUrl() const {
  if (WiFi.status() != WL_CONNECTED) {
    return {};
  }
  return "http://" + std::string(WiFi.localIP().toString().c_str()) + "/settings";
}

void OnlineUpdateActivity::loop() {
  if (state_ == State::Rebooting) {
    if (millis() - rebootAt_ > 1500) {
      ESP.restart();
    }
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (state_ != State::ServerRunning || !webServer_) {
    return;
  }

  esp_task_wdt_reset();
  for (int i = 0; i < 40 && webServer_->isRunning(); ++i) {
    webServer_->handleClient();
    if ((i & 0x07) == 0x07) {
      esp_task_wdt_reset();
    }
  }

  // A finished upload is validated and flashed from here, so the panel can show
  // progress and the HTTP request has already returned.
  if (webServer_->processPendingOta()) {
    state_ = State::Rebooting;
    rebootAt_ = millis();
    requestUpdateAndWait();
  } else if (webServer_->otaState().stage == CrossPointWebServer::OtaStage::Failed) {
    requestUpdate();
  }
}

void OnlineUpdateActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight},
                 tr(STR_ONLINE_UPDATE));

  const int left = metrics.contentSidePadding;
  const int contentWidth = width - left * 2;
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  int y = metrics.topPadding + metrics.headerHeight + 24;

  if (state_ == State::Error) {
    renderer.drawText(UI_10_FONT_ID, left, y, tr(STR_OTA_NO_WIFI));
  } else if (state_ == State::ServerStarting) {
    renderer.drawText(UI_10_FONT_ID, left, y, tr(STR_OTA_STARTING));
  } else if (state_ == State::Rebooting) {
    renderer.drawText(UI_10_FONT_ID, left, y, tr(STR_OTA_DONE));
  } else {
    renderer.drawText(UI_10_FONT_ID, left, y, tr(STR_OTA_OPEN_BROWSER));
    y += lineHeight + 8;
    const std::string url = endpointUrl();
    renderer.drawText(UI_10_FONT_ID, left, y, url.c_str());
    y += lineHeight + 12;

    const auto ota = webServer_ ? webServer_->otaState() : CrossPointWebServer::OtaState{};
    const char* stageText = tr(STR_OTA_WAITING_UPLOAD);
    if (ota.stage == CrossPointWebServer::OtaStage::Receiving) {
      stageText = tr(STR_OTA_RECEIVING);
    } else if (ota.stage == CrossPointWebServer::OtaStage::Verifying) {
      stageText = tr(STR_OTA_VERIFYING);
    } else if (ota.stage == CrossPointWebServer::OtaStage::Writing) {
      stageText = tr(STR_OTA_WRITING);
    } else if (ota.stage == CrossPointWebServer::OtaStage::Failed) {
      stageText = tr(STR_OTA_FAILED);
    } else if (ota.stage == CrossPointWebServer::OtaStage::Done) {
      stageText = tr(STR_OTA_DONE);
    }

    std::string status = stageText;
    if (ota.stage == CrossPointWebServer::OtaStage::Receiving) {
      status += " " + std::to_string(ota.percent) + "%";
    }
    renderer.drawText(UI_10_FONT_ID, left, y, status.c_str());
    y += lineHeight + 8;

    const char* hint = tr(STR_OTA_KEEP_POWER);
    renderer.drawText(SMALL_FONT_ID, left, y, renderer.truncatedText(SMALL_FONT_ID, hint, contentWidth).c_str());
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
