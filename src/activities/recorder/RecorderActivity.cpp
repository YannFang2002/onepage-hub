#include "RecorderActivity.h"

#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "components/UITheme.h"
#include "fontIds.h"

namespace {

constexpr char kRecordingsDir[] = "/record";

// If the microphone produces nothing at all for this long after capture starts,
// report a microphone fault instead of letting the user record into the void and
// then hear "recording too short". A healthy PDM stream yields samples within
// a few milliseconds, so this only ever fires on a real failure.
constexpr uint32_t kNoDataTimeoutMs = 3000;

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

// 44-byte canonical PCM WAV header: mono, 16 kHz, 16-bit.
void buildWavHeader(uint8_t header[44], uint32_t dataBytes) {
  memcpy(header, "RIFF", 4);
  writeLe32(header + 4, 36 + dataBytes);
  memcpy(header + 8, "WAVE", 4);
  memcpy(header + 12, "fmt ", 4);
  writeLe32(header + 16, 16);
  writeLe16(header + 20, 1);  // PCM
  writeLe16(header + 22, 1);  // mono
  writeLe32(header + 24, PdmMic::kSampleRate);
  writeLe32(header + 28, PdmMic::kSampleRate * 2);  // byte rate
  writeLe16(header + 32, 2);                        // block align
  writeLe16(header + 34, 16);                       // bits per sample
  memcpy(header + 36, "data", 4);
  writeLe32(header + 40, dataBytes);
}

int nextRecordingIndex() {
  const auto files = Storage.listFiles(kRecordingsDir, 200);
  int maxIndex = 0;
  for (const auto& name : files) {
    if (!name.startsWith("REC_") || !name.endsWith(".WAV")) {
      continue;
    }
    const int index = name.substring(4, name.length() - 4).toInt();
    if (index > maxIndex) {
      maxIndex = index;
    }
  }
  return maxIndex + 1;
}

}  // namespace

void RecorderActivity::onEnter() {
  Activity::onEnter();
  state_ = State::Idle;
  sampleCount_ = 0;
  filePath_[0] = '\0';
  message_[0] = '\0';
  durationSec_ = 0;
  micReady_ = false;
  captureFailed_ = false;
  // Recordings live in /record at the root of the SD card, alongside /book and
  // /system. Create it up front so the folder exists even before the first clip.
  if (Storage.ready()) {
    Storage.ensureDirectoryExists(kRecordingsDir);
  }
  requestUpdate();
}

void RecorderActivity::onExit() {
  if (taskHandle_ != nullptr || state_ == State::Recording) {
    requestStop();
  }
  freeBuffer();
  mic_.end();
  Activity::onExit();
}

void RecorderActivity::taskTrampoline(void* arg) {
  static_cast<RecorderActivity*>(arg)->captureTask();
}

void RecorderActivity::captureTask() {
  const uint32_t startedAt = millis();
  while (!stopRequested_ && sampleCount_ < kMaxSamples) {
    const int samples = mic_.read(chunk_, kChunkSamples, 50);
    if (samples < 0) {
      LOG_ERR("REC", "mic read failed");
      captureFailed_ = true;
      break;
    }
    if (samples == 0) {
      if (sampleCount_ == 0 && millis() - startedAt >= kNoDataTimeoutMs) {
        LOG_ERR("REC", "no microphone data within %u ms", static_cast<unsigned>(kNoDataTimeoutMs));
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

bool RecorderActivity::ensureBuffer() {
  if (pcm_ != nullptr) {
    return true;
  }
  pcm_ = static_cast<int16_t*>(heap_caps_malloc(kMaxSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM));
  if (pcm_ == nullptr) {
    LOG_ERR("REC", "PSRAM alloc failed (%u bytes)", static_cast<unsigned>(kMaxSamples * sizeof(int16_t)));
    return false;
  }
  return true;
}

void RecorderActivity::freeBuffer() {
  if (pcm_ != nullptr) {
    heap_caps_free(pcm_);
    pcm_ = nullptr;
  }
}

bool RecorderActivity::startRecording() {
  if (!Storage.ready()) {
    setError(tr(STR_RECORDER_NO_SD));
    return false;
  }
  if (!ensureBuffer()) {
    setError(tr(STR_RECORDER_NO_MEM));
    return false;
  }
  if (!micReady_) {
    micReady_ = mic_.begin();
  }
  if (!micReady_) {
    setError(tr(STR_RECORDER_MIC_FAIL));
    return false;
  }

  sampleCount_ = 0;
  durationSec_ = 0;
  filePath_[0] = '\0';
  stopRequested_ = false;
  taskDone_ = false;
  captureFailed_ = false;

  if (!mic_.start()) {
    setError(tr(STR_RECORDER_MIC_FAIL));
    return false;
  }
  if (xTaskCreate(&RecorderActivity::taskTrampoline, "RecMic", 4096, this, 5, &taskHandle_) != pdPASS) {
    LOG_ERR("REC", "capture task create failed");
    mic_.stop();
    taskHandle_ = nullptr;
    setError(tr(STR_RECORDER_MIC_FAIL));
    return false;
  }

  state_ = State::Recording;
  requestUpdate();
  return true;
}

void RecorderActivity::requestStop() {
  stopRequested_ = true;
  const uint32_t start = millis();
  while (!taskDone_ && millis() - start < 1500) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (!taskDone_) {
    // The task should normally exit within one read timeout; if it did not,
    // disable the channel and delete it so the DMA ring cannot keep running.
    LOG_ERR("REC", "capture task did not stop in time; forcing");
    mic_.stop();
    if (taskHandle_ != nullptr) {
      vTaskDelete(taskHandle_);
    }
    taskDone_ = true;
  }
  taskHandle_ = nullptr;
}

void RecorderActivity::finalizeRecording() {
  requestStop();

  if (sampleCount_ < static_cast<size_t>(PdmMic::kSampleRate) / 2) {
    const bool micFailed = captureFailed_;
    freeBuffer();
    // tr() is a macro taking a bare StrId token, so the choice cannot be inlined
    // into a single call.
    if (micFailed) {
      setError(tr(STR_RECORDER_MIC_FAIL));
    } else {
      setError(tr(STR_RECORDER_TOO_SHORT));
    }
    return;
  }

  state_ = State::Saving;
  requestUpdateAndWait();

  const size_t savedSamples = sampleCount_;
  const bool ok = writeWav();
  freeBuffer();

  if (!ok) {
    setError(tr(STR_RECORDER_SAVE_FAIL));
    return;
  }

  durationSec_ = static_cast<unsigned int>((savedSamples + PdmMic::kSampleRate / 2) / PdmMic::kSampleRate);
  state_ = State::Done;
  requestUpdate();
}

void RecorderActivity::cancelRecording() {
  requestStop();
  sampleCount_ = 0;
  freeBuffer();
  state_ = State::Idle;
  requestUpdate();
}

bool RecorderActivity::writeWav() {
  if (!Storage.ensureDirectoryExists(kRecordingsDir)) {
    LOG_ERR("REC", "cannot create %s", kRecordingsDir);
    return false;
  }

  char path[48];
  snprintf(path, sizeof(path), "%s/REC_%04d.WAV", kRecordingsDir, nextRecordingIndex());

  uint8_t header[44];
  const uint32_t payloadBytes = static_cast<uint32_t>(sampleCount_ * sizeof(int16_t));
  buildWavHeader(header, payloadBytes);

  HalFile file;
  if (!Storage.openFileForWrite("REC", path, file)) {
    return false;
  }

  size_t written = file.write(header, sizeof(header));
  written += file.write(pcm_, payloadBytes);
  file.flush();

  if (written != sizeof(header) + payloadBytes) {
    LOG_ERR("REC", "short write: %u/%u", static_cast<unsigned>(written),
            static_cast<unsigned>(sizeof(header) + payloadBytes));
    file.close();
    Storage.remove(path);
    return false;
  }

  strncpy(filePath_, path, sizeof(filePath_) - 1);
  filePath_[sizeof(filePath_) - 1] = '\0';
  LOG_INF("REC", "saved %s (%u bytes, %u s)", filePath_, static_cast<unsigned>(payloadBytes),
          static_cast<unsigned>((sampleCount_ + PdmMic::kSampleRate / 2) / PdmMic::kSampleRate));
  return true;
}

void RecorderActivity::setError(const char* message) {
  state_ = State::Error;
  snprintf(message_, sizeof(message_), "%s", message != nullptr ? message : "");
  requestUpdate();
}

void RecorderActivity::loop() {
  if (state_ == State::Recording) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      cancelRecording();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) || taskDone_) {
      finalizeRecording();
      return;
    }
    return;
  }

  if (state_ == State::Saving) {
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    startRecording();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onGoDesktop();
  }
}

void RecorderActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_VOICE_RECORDER));

  const char* primary = "";
  const char* secondary = "";
  const char* tertiary = "";
  const char* quaternary = "";
  char duration[64];

  switch (state_) {
    case State::Idle:
      primary = tr(STR_RECORDER_READY);
      secondary = tr(STR_RECORDER_MAX_HINT);
      if (!Storage.ready()) {
        tertiary = tr(STR_RECORDER_NO_SD);
      }
      break;
    case State::Recording:
      primary = tr(STR_RECORDER_RECORDING);
      secondary = tr(STR_RECORDER_STOP_HINT);
      break;
    case State::Saving:
      primary = tr(STR_RECORDER_SAVING);
      break;
    case State::Done:
      primary = tr(STR_RECORDER_SAVED);
      snprintf(duration, sizeof(duration), tr(STR_RECORDER_DURATION), durationSec_);
      secondary = duration;
      tertiary = filePath_;
      quaternary = tr(STR_RECORDER_AGAIN);
      break;
    case State::Error:
      primary = message_;
      break;
  }

  const int y = metrics.topPadding + metrics.headerHeight + 90;
  renderer.drawCenteredText(UI_12_FONT_ID, y, primary, true, EpdFontFamily::BOLD);
  if (secondary[0] != '\0') {
    renderer.drawCenteredText(UI_10_FONT_ID, y + 48, secondary, true);
  }
  if (tertiary[0] != '\0') {
    renderer.drawCenteredText(UI_10_FONT_ID, y + 78, tertiary, true);
  }
  if (quaternary[0] != '\0') {
    renderer.drawCenteredText(UI_10_FONT_ID, y + 120, quaternary, true);
  }

  const char* backLabel = tr(STR_BACK);
  const char* confirmLabel = tr(STR_RECORDER_START);
  if (state_ == State::Recording) {
    backLabel = tr(STR_CANCEL);
    confirmLabel = tr(STR_RECORDER_STOP);
  } else if (state_ == State::Saving) {
    backLabel = "";
    confirmLabel = "";
  } else if (state_ == State::Error) {
    confirmLabel = tr(STR_RETRY);
  }

  const auto labels = mappedInput.mapLabels(backLabel, confirmLabel, "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
