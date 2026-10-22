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
  // The app has two jobs now: talk to the host's agent (with a screen), or act
  // as a wireless microphone. The chooser runs on every entry.
  enum class Mode { Choose, Agent, Microphone };

  Mode mode_ = Mode::Choose;
  int modeIndex_ = 0;

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
  // Last liveness ping. The host marks the device offline when this stops
  // arriving (app closed, WiFi off, deep sleep).
  unsigned long lastHeartbeatMs_ = 0;
  // Last status poll. The page used to fetch only on entry, so the agent state
  // stayed stale ("空闲" while the host reported running).
  unsigned long lastStatusPollMs_ = 0;

  bool ensureNetwork();
  bool waitForNetwork();
  // Everything the agent mode needs on entry: config, network, discovery/poll.
  void enterAgentMode();
  void startWifiSetup();
  void refreshStatus();
  // Light periodic poll: short timeout, redraws only when something changed.
  void pollStatus();
  bool discoverHost();
  void announceHost();
  void startVoice();
  void finishVoice();
  void cancelVoice();
  void startHostInput();
  bool sendMessage(const std::string& text);
  void applyHost(const std::string& input);
};
