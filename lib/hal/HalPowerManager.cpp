#include "HalPowerManager.h"

#include <Logging.h>
#include <WiFi.h>
#include <esp_sleep.h>

#include <algorithm>
#include <cassert>

#include "HalGPIO.h"

HalPowerManager powerManager;  // Singleton instance

volatile bool HalPowerManager::bleActive_ = false;

namespace {

// ADC sampling of the pack divider (OnePage / X4).
//
// The divider node is high impedance and starts far above the battery voltage
// after power-on: a probe on the real board read 3005 mV decaying to 1848 mV
// with a ~0.35 s time constant. Feeding that transient into the percentage curve
// pegged the status bar at 100% on a half-flat pack, and because the value was
// only recomputed when the status bar was redrawn it then stepped down as the
// user operated the device.
//
// A settled node is quiet, so a burst whose spread is too wide means the node is
// still moving and the whole burst is dropped.
constexpr uint8_t kBatterySamples = 5;
constexpr uint16_t kBatterySampleGapMs = 12;  // the burst spans ~60 ms
constexpr uint16_t kBatteryMaxSpreadMv = 40;  // wider than this: still settling
constexpr uint16_t kBatteryMinPackMv = 2900;  // ~2.9 V: pack deeply discharged
constexpr uint16_t kBatteryMaxPackMv = 4350;  // ~4.35 V: impossible for one cell

// Median of a stable burst, or 0 when the node is still settling / the value is
// not a plausible single-cell voltage.
uint16_t samplePackMillivolts(const BatteryMonitor& battery) {
  uint16_t samples[kBatterySamples];
  uint16_t lo = 0xFFFF;
  uint16_t hi = 0;
  for (uint8_t i = 0; i < kBatterySamples; i++) {
    const uint16_t mv = battery.readMillivolts();
    samples[i] = mv;
    if (mv < lo) lo = mv;
    if (mv > hi) hi = mv;
    delay(kBatterySampleGapMs);
  }
  if (hi - lo > kBatteryMaxSpreadMv) return 0;
  std::sort(samples, samples + kBatterySamples);
  const uint16_t median = samples[kBatterySamples / 2];
  if (median < kBatteryMinPackMv || median > kBatteryMaxPackMv) return 0;
  return median;
}

}  // namespace

void HalPowerManager::begin() {
  if (gpio.deviceIsX3()) {
    // X3 uses an I2C fuel gauge for battery monitoring.
    // I2C init must come AFTER gpio.begin() so early hardware detection/probes are finished.
    Wire.begin(X3_I2C_SDA, X3_I2C_SCL, X3_I2C_FREQ);
    Wire.setTimeOut(4);
    _batteryUseI2C = true;
  } else {
    pinMode(BAT_GPIO0, INPUT);
#ifdef ONEPAGE_C61
    // Keep the charger enabled. The sampler no longer toggles BAT_CHG_EN: pausing
    // the charger moved the divider node by only ~30 mV, which does not justify
    // churning the charge path on every poll.
    pinMode(BAT_CHG_EN, OUTPUT);
    digitalWrite(BAT_CHG_EN, HIGH);
#endif
  }
  normalFreq = getCpuFrequencyMhz();
  currentSetFreq = normalFreq;
  modeMutex = xSemaphoreCreateMutex();
  assert(modeMutex != nullptr);
}

void HalPowerManager::setPowerSaving(bool enabled) {
  if (normalFreq <= 0) {
    return;  // invalid state
  }

  auto wifiMode = WiFi.getMode();
  if (wifiMode != WIFI_MODE_NULL) {
    // Wifi is active, force disabling power saving
    enabled = false;
  }

  // Note: We don't use mutex here to avoid too much overhead,
  // it's not very important if we read a slightly stale value for currentLockMode
  const LockMode mode = currentLockMode;

  // Decide the target CPU frequency by priority:
  //   Lock (perf-critical task) / WiFi  -> full speed (normalFreq)
  //   BLE link up (no WiFi/lock)         -> BLE_ACTIVE_FREQ (80 MHz): low enough
  //                                         to cut CPU power, high enough to keep
  //                                         the BLE radio timing stable (10 MHz
  //                                         trips the interrupt watchdog).
  //   idle power-saving requested        -> LOW_POWER_FREQ (10 MHz)
  //   otherwise                          -> full speed
  int targetFreq;
  if (mode != None || !enabled) {
    // A lock is held, or the caller does not want power saving right now.
    // BLE still gets its 80 MHz clamp (a link being up is not "full speed"
    // work), but a NormalSpeed lock or WiFi overrides everything.
    targetFreq = (bleActive_ && mode == None && wifiMode == WIFI_MODE_NULL) ? BLE_ACTIVE_FREQ : normalFreq;
  } else {
    // enabled && no lock: idle. Drop to 10 MHz, unless a BLE link forbids it.
    targetFreq = bleActive_ ? BLE_ACTIVE_FREQ : LOW_POWER_FREQ;
  }

  const int currentFreq = currentSetFreq > 0 ? currentSetFreq : normalFreq;
  if (targetFreq == currentFreq) {
    return;  // no change needed
  }

  LOG_DBG("PWR", "CPU frequency -> %d MHz", targetFreq);
  if (!setCpuFrequencyMhz(targetFreq)) {
    LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", targetFreq);
    return;
  }
  currentSetFreq = targetFreq;
}

void HalPowerManager::startDeepSleep(HalGPIO& gpio) const {
  // Ensure that the power button has been released to avoid immediately turning back on if you're holding it
  while (gpio.isPressed(HalGPIO::BTN_POWER)) {
    delay(50);
    gpio.update();
  }

#ifdef ENABLE_SERIAL_LOG
  // Tear down HWCDC so the host sees a clean disconnect and the peripheral
  // doesn't hold power domains that interfere with USB-powered GPIO wake.
  // logSerial is the raw HWCDC reference; Serial is the MySerialImpl proxy
  // (which doesn't expose end()).
  logSerial.end();
#endif

  // OnePage: cut the SD/MIC/EPD power rail (GPIO27) and silence the shared
  // SPI/PDM lines so those peripherals stop drawing current in deep sleep and
  // can't back-power the cut rail. No-op on X4/X3. The EPD was already parked
  // by display.deepSleep() before this call (see main.cpp goToDeepSleep).
  gpio.powerDownPeripheralsForDeepSleep();

  // Pre-sleep routines from the original firmware
  // GPIO13 is connected to battery latch MOSFET, we need to make sure it's low during sleep
  // Note that this means the MCU will be completely powered off during sleep, including RTC
#ifndef ONEPAGE_C61
  // GPIO13 is the X4 battery-latch MOSFET. On OnePage GPIO13 = USB_D+, so
  // driving it low would break USB-Serial-JTAG — skip this whole block there.
  constexpr gpio_num_t GPIO_SPIWP = GPIO_NUM_13;
  gpio_set_direction(GPIO_SPIWP, GPIO_MODE_OUTPUT);
  gpio_set_level(GPIO_SPIWP, 0);
  esp_sleep_config_gpio_isolate();
  gpio_deep_sleep_hold_en();
  gpio_hold_en(GPIO_SPIWP);
#endif
  pinMode(InputManager::POWER_BUTTON_PIN, INPUT_PULLUP);
  // Arm the wakeup trigger *after* the button is released
  // Note: this is only useful for waking up on USB power. On battery, the MCU will be completely powered off, so the
  // power button is hard-wired to briefly provide power to the MCU, waking it up regardless of the wakeup source
  // configuration
  esp_deep_sleep_enable_gpio_wakeup(1ULL << InputManager::POWER_BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
  // Enter Deep Sleep
  esp_deep_sleep_start();
}

uint16_t HalPowerManager::getBatteryPercentage() const {
  const unsigned long now = millis();
  // A redraw asks for the percentage twice (once for the layout, once for the
  // icon); the short window makes both calls share one sample without ever
  // showing a stale value on a later redraw.
  if (_batteryLastPollMs != 0 && (now - _batteryLastPollMs) < BATTERY_POLL_MS) {
    return _batteryReturnedPercent;
  }
  _batteryLastPollMs = now;

  if (_batteryUseI2C) {
    // Read SOC directly from I2C fuel gauge (16-bit LE register).
    // On I2C error, keep last known value to avoid UI jitter/slowdowns.
    Wire.beginTransmission(I2C_ADDR_BQ27220);
    Wire.write(BQ27220_SOC_REG);
    if (Wire.endTransmission(false) != 0) {
      return _batteryReturnedPercent;
    }
    Wire.requestFrom(I2C_ADDR_BQ27220, (uint8_t)2);
    if (Wire.available() < 2) {
      return _batteryReturnedPercent;
    }
    const uint8_t lo = Wire.read();
    const uint8_t hi = Wire.read();
    const uint16_t soc = (hi << 8) | lo;
    _batteryCachedPercent = soc > 100 ? 100 : soc;
    _batteryReturnedPercent = _batteryCachedPercent;
    return _batteryReturnedPercent;
  }

  // OnePage / X4: voltage divider on an ADC pin, converted with the original
  // voltage -> percentage curve.
  static const BatteryMonitor battery = BatteryMonitor(BAT_GPIO0);
  const uint16_t mv = samplePackMillivolts(battery);
  if (mv == 0) {
    // Node still settling or a bad sample: keep the last value and let the next
    // call sample again straight away.
    _batteryLastPollMs = now - BATTERY_POLL_MS;
    return _batteryReturnedPercent;
  }

  // Report the pack as it is right now. A redraw is rare (it costs a full e-ink
  // refresh), so there is nothing to smooth: smoothing only made the number
  // crawl towards the truth one redraw at a time.
  _batteryReturnedPercent = BatteryMonitor::percentageFromMillivolts(mv);
  LOG_DBG("PWR", "Battery %umV -> %u%%", mv, _batteryReturnedPercent);
  return _batteryReturnedPercent;
}

HalPowerManager::Lock::Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  // Current limitation: only one lock at a time
  if (powerManager.currentLockMode != None) {
    LOG_ERR("PWR", "Lock already held, ignore");
    valid = false;
  } else {
    powerManager.currentLockMode = NormalSpeed;
    valid = true;
  }
  xSemaphoreGive(powerManager.modeMutex);
  if (valid) {
    // Immediately restore normal CPU frequency if currently in low-power mode
    powerManager.setPowerSaving(false);
  }
}

HalPowerManager::Lock::~Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  if (valid) {
    powerManager.currentLockMode = None;
  }
  xSemaphoreGive(powerManager.modeMutex);
}
