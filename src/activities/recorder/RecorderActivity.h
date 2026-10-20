#pragma once

#include <PdmMic.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "activities/Activity.h"

// Voice recorder: captures 16 kHz mono PCM from the PDM microphone into a PSRAM
// buffer while the user talks, then writes it as a WAV file under /Recordings on
// the SD card. Capture runs in its own task so a slow UI/e-ink operation can
// never starve the I2S DMA ring.
class RecorderActivity final : public Activity {
 public:
  explicit RecorderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Recorder", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  bool preventAutoSleep() override { return state_ == State::Recording || state_ == State::Saving; }

 private:
  enum class State { Idle, Recording, Saving, Done, Error };

  static constexpr int kMaxSeconds = 15;
  static constexpr size_t kMaxSamples = static_cast<size_t>(kMaxSeconds) * PdmMic::kSampleRate;
  static constexpr size_t kChunkSamples = PdmMic::kMaxFrameSamples;

  PdmMic mic_;
  int16_t* pcm_ = nullptr;
  int16_t chunk_[kChunkSamples] = {};
  size_t sampleCount_ = 0;

  State state_ = State::Idle;
  char message_[80] = {};
  char filePath_[48] = {};
  unsigned int durationSec_ = 0;

  volatile bool stopRequested_ = false;
  volatile bool taskDone_ = false;
  TaskHandle_t taskHandle_ = nullptr;
  bool micReady_ = false;

  static void taskTrampoline(void* arg);
  void captureTask();

  bool ensureBuffer();
  void freeBuffer();
  bool startRecording();
  void requestStop();
  void finalizeRecording();
  void cancelRecording();
  bool writeWav();
  void setError(const char* message);
};
