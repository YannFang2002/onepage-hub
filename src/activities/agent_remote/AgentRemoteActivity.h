#pragma once

#include <string>

#include "activities/Activity.h"
#include "agent/AgentConfig.h"
#include "agent/VoiceCapture.h"
#include "network/AgentClient.h"

// Agent Remote app:
//   status page -> hold-to-talk -> WAV -> host ASR -> preview -> send.
// Host is auto-discovered over UDP 8767 when none is configured.
class AgentRemoteActivity final : public Activity {
 public:
  explicit AgentRemoteActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("AgentRemote", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  bool preventAutoSleep() override { return recording_ || transcribing_ || discovering_; }
  bool skipLoopDelay() override { return recording_ || transcribing_ || discovering_; }

 private:
  AgentConfig config_;
  AgentStatus status_;
  VoiceCapture voice_;
  std::string lastReply_;
  std::string error_;
  std::string transcript_;

  bool configured_ = false;
  bool loading_ = false;
  bool sending_ = false;
  bool wifiConnecting_ = false;
  bool recording_ = false;
  bool transcribing_ = false;
  bool preview_ = false;
  bool discovering_ = false;
  bool discoveryTried_ = false;
  unsigned long wifiStartMs_ = 0;
  unsigned long recordStartMs_ = 0;
  unsigned long lastRecordUiMs_ = 0;

  bool ensureNetwork();
  bool waitForNetwork();
  void startWifiSetup();
  void refreshStatus();
  bool discoverHost();
  void announceHost();
  void startVoice();
  void finishVoice();
  void cancelVoice();
  void startHostInput();
  bool sendMessage(const std::string& text);
  void applyHost(const std::string& input);
};
