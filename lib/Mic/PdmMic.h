#pragma once

#include <driver/i2s_pdm.h>

#include <cstddef>
#include <cstdint>

// PDM microphone capture for the OnePage C61 board.
//
// The ESP32-C61 has no hardware PDM-to-PCM converter, so the I2S peripheral is
// configured in raw PDM mode and the PDM bitstream is decimated in software:
// 3rd-order CIC (R=128) at a 2.048 MHz oversample -> 16 kHz mono PCM, followed
// by a DC blocker, gain, 1st-order low-pass and a soft limiter.
//
// Ported from MoveCall/bsp_onepage_c61 board_mic.c (same board, same pins).
// Keep the DMA ring small (~10 KB): the BSP notes 32 KB starves Wi-Fi's
// internal RAM. Reads must never stall long enough to overflow the ring, so the
// caller should run a dedicated capture task rather than reading in a slow UI
// loop.
class PdmMic {
 public:
  static constexpr int kSampleRate = 16000;
  static constexpr int kMaxFrameSamples = 512;

  PdmMic() = default;
  ~PdmMic();

  PdmMic(const PdmMic&) = delete;
  PdmMic& operator=(const PdmMic&) = delete;

  bool begin();
  void end();
  bool start();
  bool stop();

  [[nodiscard]] bool ready() const { return rx_ != nullptr; }

  // Reads one DMA buffer and decodes it. Returns the number of 16-bit PCM
  // samples written to `out` (0 on timeout, -1 on error).
  int read(int16_t* out, size_t maxSamples, uint32_t timeoutMs);

 private:
  static constexpr int kRawReadWords = 2048;  // 4 KB read, ~8 ms of audio
  static constexpr int kStride = 2;           // DMA delivers pair-duplicated words
  static constexpr int kCicR = 128;           // 4.096 MHz PDM -> 16 kHz PCM
  static constexpr int64_t kCicR3 = static_cast<int64_t>(kCicR) * kCicR * kCicR;
  static constexpr int32_t kOutGain = 120000;
  static constexpr int kLpNum = 11;

  i2s_chan_handle_t rx_ = nullptr;
  int16_t raw_[kRawReadWords] = {};

  // Decoder state (reset on every start()).
  uint32_t cic1_ = 0;
  uint32_t cic2_ = 0;
  uint32_t cic3_ = 0;
  uint32_t comb1_ = 0;
  uint32_t comb2_ = 0;
  uint32_t comb3_ = 0;
  int cicCount_ = 0;
  int32_t hpXm1_ = 0;
  int32_t hpYm1_ = 0;
  int32_t lp1_ = 0;

  void resetDecoder();
  int decode(const int16_t* raw, size_t bytes, int16_t* out, size_t maxSamples);
};
