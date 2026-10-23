#pragma once

#include <string>

#include "activities/Activity.h"
#include "agent/AgentConfig.h"

// Online update over the LAN: ask the host bridge (the same address the agent
// uses) for the newest firmware, download it from there, then hand the image to
// FirmwareFlasher. No cloud, no GitHub, nothing outside the local network.
class OnlineUpdateActivity final : public Activity {
 public:
  explicit OnlineUpdateActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("OnlineUpdate", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class Phase { Idle, Checking, UpToDate, Downloading, Verifying, Writing, Done, Failed };

  AgentConfig config_;
  Phase phase_ = Phase::Idle;
  std::string message_;
  std::string remoteVersion_;
  std::string downloadUrl_;
  int percent_ = 0;
  bool workPending_ = false;
  bool rebootPending_ = false;

  static constexpr const char* kStagingPath = "/system/.crosspoint/ota/firmware.bin";

  void setPhase(Phase phase, const std::string& message);
  void runUpdate();
  bool fetchManifest();
  bool downloadImage();
  bool flashImage();
};
