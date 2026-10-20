#include "PdmMic.h"

#include <Logging.h>
#include <driver/i2s_common.h>

namespace {
constexpr int kPdmClkPin = 7;
constexpr int kPdmDinPin = 3;
constexpr int kOversampleHz = 2048000;
}  // namespace

PdmMic::~PdmMic() { end(); }

void PdmMic::resetDecoder() {
  cic1_ = cic2_ = cic3_ = 0;
  comb1_ = comb2_ = comb3_ = 0;
  cicCount_ = 0;
  hpXm1_ = hpYm1_ = lp1_ = 0;
}

bool PdmMic::begin() {
  if (rx_ != nullptr) {
    return true;
  }

  i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
  // ~10 KB ring (~20 ms) so a decode pass or a short preemption gap cannot
  // starve it. 8 x 640 frames is the value validated on this board.
  chanCfg.dma_desc_num = 8;
  chanCfg.dma_frame_num = 640;

  esp_err_t err = i2s_new_channel(&chanCfg, nullptr, &rx_);
  if (err != ESP_OK) {
    LOG_ERR("MIC", "i2s_new_channel failed: %s", esp_err_to_name(err));
    rx_ = nullptr;
    return false;
  }

  i2s_pdm_rx_config_t cfg = {};
  cfg.clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(kOversampleHz);
  cfg.slot_cfg = I2S_PDM_RX_SLOT_RAW_FMT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
  cfg.gpio_cfg.clk = static_cast<gpio_num_t>(kPdmClkPin);
  cfg.gpio_cfg.din = static_cast<gpio_num_t>(kPdmDinPin);
  cfg.gpio_cfg.invert_flags.clk_inv = false;

  err = i2s_channel_init_pdm_rx_mode(rx_, &cfg);
  if (err != ESP_OK) {
    LOG_ERR("MIC", "PDM RX init failed: %s", esp_err_to_name(err));
    i2s_del_channel(rx_);
    rx_ = nullptr;
    return false;
  }

  LOG_INF("MIC", "PDM mic ready (CLK=%d DIN=%d, %d Hz PCM)", kPdmClkPin, kPdmDinPin, kSampleRate);
  return true;
}

void PdmMic::end() {
  if (rx_ == nullptr) {
    return;
  }
  i2s_channel_disable(rx_);
  i2s_del_channel(rx_);
  rx_ = nullptr;
}

bool PdmMic::start() {
  if (rx_ == nullptr) {
    return false;
  }
  resetDecoder();
  const esp_err_t err = i2s_channel_enable(rx_);
  if (err != ESP_OK) {
    LOG_ERR("MIC", "i2s_channel_enable failed: %s", esp_err_to_name(err));
    return false;
  }
  return true;
}

bool PdmMic::stop() {
  if (rx_ == nullptr) {
    return false;
  }
  const esp_err_t err = i2s_channel_disable(rx_);
  if (err != ESP_OK) {
    LOG_ERR("MIC", "i2s_channel_disable failed: %s", esp_err_to_name(err));
    return false;
  }
  return true;
}

int PdmMic::read(int16_t* out, size_t maxSamples, uint32_t timeoutMs) {
  if (rx_ == nullptr || out == nullptr || maxSamples == 0) {
    return -1;
  }

  size_t bytesRead = 0;
  const esp_err_t err = i2s_channel_read(rx_, raw_, sizeof(raw_), &bytesRead, timeoutMs);
  if (err == ESP_ERR_TIMEOUT) {
    return 0;
  }
  if (err != ESP_OK) {
    return 0;
  }
  if (bytesRead == 0) {
    return 0;
  }
  return decode(raw_, bytesRead, out, maxSamples);
}

int PdmMic::decode(const int16_t* raw, size_t bytes, int16_t* out, size_t maxSamples) {
  const auto* words = reinterpret_cast<const uint16_t*>(raw);
  const int wordCount = static_cast<int>(bytes / 2);
  int produced = 0;

  for (int iw = 0; iw < wordCount; iw += kStride) {
    const uint16_t word = words[iw];
    for (int bit = 15; bit >= 0; --bit) {
      const uint32_t one = (word >> bit) & 1U;
      cic1_ += one;
      cic2_ += cic1_;
      cic3_ += cic2_;
      if (++cicCount_ < kCicR) {
        continue;
      }
      cicCount_ = 0;

      const uint32_t v = cic3_;
      const uint32_t d1 = v - comb1_;
      comb1_ = v;
      const uint32_t d2 = d1 - comb2_;
      comb2_ = d1;
      const uint32_t d3 = d2 - comb3_;
      comb3_ = d2;

      const int64_t centered = static_cast<int64_t>(d3) * 2 - kCicR3;
      const int32_t x = static_cast<int32_t>(centered * kOutGain / kCicR3);

      // DC blocker (1st-order high-pass).
      int32_t y = x - hpXm1_ + hpYm1_ - (hpYm1_ >> 6);
      hpXm1_ = x;
      hpYm1_ = y;

      // 1st-order low-pass, alpha = 11/16: cuts PDM hiss.
      lp1_ += ((y - lp1_) * kLpNum) >> 4;

      // Soft limiter: compress peaks toward +/-32000 instead of hard clipping.
      constexpr int32_t kKnee = 22000;
      constexpr int32_t kHeadroom = 10000;
      int32_t s = lp1_;
      if (s > kKnee) {
        const int32_t e = s - kKnee;
        s = kKnee + static_cast<int32_t>(static_cast<int64_t>(e) * kHeadroom / (e + kHeadroom));
      } else if (s < -kKnee) {
        const int32_t e = -s - kKnee;
        s = -(kKnee + static_cast<int32_t>(static_cast<int64_t>(e) * kHeadroom / (e + kHeadroom)));
      }

      out[produced++] = static_cast<int16_t>(s);
      if (static_cast<size_t>(produced) >= maxSamples) {
        return produced;
      }
    }
  }

  return produced;
}
