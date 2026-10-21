#pragma once

#include <PdmMic.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstddef>
#include <cstdint>
#include <string>

// Push-to-talk capture for the Agent Remote app.
// Records 16 kHz mono PCM into PSRAM on a dedicated task, then builds a WAV in
// memory for a single HTTP POST. No SD write is involved.
class VoiceCapture {
 public:
  static constexpr int kMaxSeconds = 15;
  static constexpr size_t kMaxSamples = static_cast<size_t>(kMaxSeconds) * PdmMic::kSampleRate;
  static constexpr size_t kMinSamples = PdmMic::kSampleRate / 2;  // 0.5 s
  static constexpr size_t kChunkSamples = PdmMic::kMaxFrameSamples;

  VoiceCapture() = default;
  ~VoiceCapture();

  VoiceCapture(const VoiceCapture&) = delete;
  VoiceCapture& operator=(const VoiceCapture&) = delete;

  bool ensure();
  bool start();
  void stop();
  bool buildWav(std::string& out);
  void release();

  [[nodiscard]] bool recording() const { return recording_; }
  [[nodiscard]] size_t samples() const { return sampleCount_; }
  [[nodiscard]] bool failed() const { return captureFailed_; }

 private:
  PdmMic mic_;
  int16_t* pcm_ = nullptr;
  int16_t chunk_[kChunkSamples] = {};
  size_t sampleCount_ = 0;

  volatile bool stopRequested_ = false;
  volatile bool taskDone_ = false;
  volatile bool captureFailed_ = false;
  bool recording_ = false;
  bool micReady_ = false;
  bool bufferReady_ = false;
  TaskHandle_t taskHandle_ = nullptr;

  static void taskTrampoline(void* arg);
  void captureTask();
};
