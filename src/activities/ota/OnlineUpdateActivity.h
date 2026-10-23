#pragma once

#include <memory>
#include <string>

#include "activities/Activity.h"

class CrossPointWebServer;

// Online update over the LAN: this app just hosts the device's own web server and
// exposes /ota, so a phone or PC on the same network can upload a firmware image.
// The flashing itself is done by FirmwareFlasher from this activity's loop.
class OnlineUpdateActivity final : public Activity {
 public:
  explicit OnlineUpdateActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("OnlineUpdate", renderer, mappedInput) {}
  ~OnlineUpdateActivity() override;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class State { WifiWait, ServerStarting, ServerRunning, Rebooting, Error };

  State state_ = State::WifiWait;
  std::unique_ptr<CrossPointWebServer> webServer_;
  unsigned long rebootAt_ = 0;

  void startServer();
  void stopServer();
  std::string endpointUrl() const;
};
