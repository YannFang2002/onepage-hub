#include "VoiceCapture.h"

#include <Logging.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cstring>

namespace {

void writeLe16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

void writeLe32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
  p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
  p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

void buildWavHeader(uint8_t header[44], uint32_t dataBytes) {
  memcpy(header, "RIFF", 4);
  writeLe32(header + 4, 36 + dataBytes);
  memcpy(header + 8, "WAVE", 4);
  memcpy(header + 12, "fmt ", 4);
  writeLe32(header + 16, 16);
  writeLe16(header + 20, 1);  // PCM
  writeLe16(header + 22, 1);  // mono
  writeLe32(header + 24, PdmMic::kSampleRate);
  writeLe32(header + 28, PdmMic::kSampleRate * 2);
  writeLe16(header + 32, 2);
  writeLe16(header + 34, 16);
  memcpy(header + 36, "data", 4);
  writeLe32(header + 40, dataBytes);
}

constexpr uint32_t kNoDataTimeoutMs = 3000;
constexpr uint32_t kStopTimeoutMs = 1500;

}  // namespace

VoiceCapture::~VoiceCapture() { release(); }

bool VoiceCapture::ensure() {
  if (!bufferReady_) {
    pcm_ = static_cast<int16_t*>(heap_caps_malloc(kMaxSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM));
    if (pcm_ == nullptr) {
      LOG_ERR("VOICE", "PSRAM alloc failed (%u bytes)", static_cast<unsigned>(kMaxSamples * sizeof(int16_t)));
      return false;
    }
    bufferReady_ = true;
  }
  if (!micReady_) {
    micReady_ = mic_.begin();
    if (!micReady_) {
      LOG_ERR("VOICE", "microphone init failed");
    }
  }
  return bufferReady_ && micReady_;
}

bool VoiceCapture::start() {
  if (recording_) {
    return false;
  }
  if (!ensure()) {
    return false;
  }

  sampleCount_ = 0;
  stopRequested_ = false;
  taskDone_ = false;
  captureFailed_ = false;

  if (!mic_.start()) {
    LOG_ERR("VOICE", "microphone start failed");
    return false;
  }
  if (xTaskCreate(&VoiceCapture::taskTrampoline, "VoiceCap", 4096, this, 5, &taskHandle_) != pdPASS) {
    LOG_ERR("VOICE", "capture task create failed");
    mic_.stop();
    taskHandle_ = nullptr;
    return false;
  }

  recording_ = true;
  return true;
}

void VoiceCapture::stop() {
  if (!recording_) {
    return;
  }
  stopRequested_ = true;
  const uint32_t start = millis();
  while (!taskDone_ && millis() - start < kStopTimeoutMs) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (!taskDone_) {
    LOG_ERR("VOICE", "capture task did not stop; forcing");
    mic_.stop();
    if (taskHandle_ != nullptr) {
      vTaskDelete(taskHandle_);
    }
    taskDone_ = true;
  }
  taskHandle_ = nullptr;
  recording_ = false;
}

bool VoiceCapture::buildWav(std::string& out) {
  out.clear();
  stop();

  if (captureFailed_) {
    return false;
  }
  if (sampleCount_ < kMinSamples) {
    return false;
  }

  const size_t dataBytes = sampleCount_ * sizeof(int16_t);
  uint8_t header[44];
  buildWavHeader(header, static_cast<uint32_t>(dataBytes));

  out.reserve(44 + dataBytes);
  out.append(reinterpret_cast<const char*>(header), sizeof(header));
  out.append(reinterpret_cast<const char*>(pcm_), dataBytes);
  return true;
}

void VoiceCapture::release() {
  stop();
  if (pcm_ != nullptr) {
    heap_caps_free(pcm_);
    pcm_ = nullptr;
  }
  bufferReady_ = false;
  mic_.end();
  micReady_ = false;
}

void VoiceCapture::taskTrampoline(void* arg) { static_cast<VoiceCapture*>(arg)->captureTask(); }

void VoiceCapture::captureTask() {
  const uint32_t startedAt = millis();
  while (!stopRequested_ && sampleCount_ < kMaxSamples) {
    const int samples = mic_.read(chunk_, kChunkSamples, 50);
    if (samples < 0) {
      LOG_ERR("VOICE", "mic read failed");
      captureFailed_ = true;
      break;
    }
    if (samples == 0) {
      if (sampleCount_ == 0 && millis() - startedAt >= kNoDataTimeoutMs) {
        LOG_ERR("VOICE", "no microphone data");
        captureFailed_ = true;
        break;
      }
      continue;
    }
    const size_t remaining = kMaxSamples - sampleCount_;
    const size_t take = std::min(static_cast<size_t>(samples), remaining);
    memcpy(pcm_ + sampleCount_, chunk_, take * sizeof(int16_t));
    sampleCount_ += take;
  }

  mic_.stop();
  taskDone_ = true;
  vTaskDelete(nullptr);
}
