#pragma once

#if defined(ESP32S3)
#include <M5Unified.h>
#include <math.h>
#include <sys/time.h>
#include <time.h>

using TFT_eSPI = lgfx::LGFXBase;
using TFT_eSprite = lgfx::LGFX_Sprite;

struct RTC_TimeTypeDef {
  uint8_t Hours;
  uint8_t Minutes;
  uint8_t Seconds;
};

struct RTC_DateTypeDef {
  uint8_t WeekDay;
  uint8_t Month;
  uint8_t Date;
  uint16_t Year;
};

#define DEVICE_HAS_BATTERY_CURRENT 0
#define DEVICE_HAS_ALERT_LED 1
#else
#include <M5StickCPlus.h>
#define DEVICE_HAS_BATTERY_CURRENT 1
#define DEVICE_HAS_ALERT_LED 1
#endif

namespace device {
#if defined(ESP32S3)
inline lgfx::LGFX_Device& lcd() { return M5.Display; }

inline void begin() {
  Serial.setRxBufferSize(4096);
  Serial.setTxTimeoutMs(0);
  Serial.begin(115200);
  auto config = M5.config();
  config.fallback_board = m5::board_t::board_M5StickS3;
  config.internal_rtc = false;
  config.output_power = false;
  M5.begin(config);
}

inline void initImu() {
  if (!M5.Imu.isEnabled()) M5.Imu.begin(nullptr, m5::board_t::board_M5StickS3);
}

inline void getAccelData(float* ax, float* ay, float* az) { M5.Imu.getAccelData(ax, ay, az); }
inline float batteryVoltage() { return M5.Power.getBatteryVoltage() / 1000.0f; }
inline int batteryCurrent() { return M5.Power.getBatteryCurrent(); }
inline float vbusVoltage() { return M5.Power.getVBUSVoltage() / 1000.0f; }
inline bool isCharging() { return M5.Power.isCharging() == m5::Power_Class::is_charging; }
inline uint8_t powerButtonPress() { return M5.Power.getKeyState(); }
inline void powerOff() { M5.Power.powerOff(); }

inline float temperatureC() {
  float value = 0.0f;
  return M5.Imu.getTemp(&value) ? value : NAN;
}

inline uint8_t& brightnessPercent() {
  static uint8_t value = 80;
  return value;
}

inline bool& screenEnabled() {
  static bool value = true;
  return value;
}

inline void screenBreath(uint8_t percent) {
  brightnessPercent() = percent;
  if (screenEnabled()) M5.Display.setBrightness((percent * 255U) / 100U);
}

inline void setScreenPower(bool enabled) {
  screenEnabled() = enabled;
  M5.Display.setBrightness(enabled ? (brightnessPercent() * 255U) / 100U : 0);
}

inline void beginAudio() { M5.Speaker.begin(); }
inline void updateAudio() {}
inline void tone(uint16_t frequency, uint16_t duration) { M5.Speaker.tone(frequency, duration); }
inline bool& alertLedState() {
  static bool value = false;
  return value;
}

inline void initAlertLed(int) {
  alertLedState() = false;
  M5.Power.M5pm1.setLedEnLevel(false);
}

inline void setAlertLed(bool enabled) {
  if (enabled == alertLedState()) return;
  if (M5.Power.M5pm1.setLedEnLevel(enabled)) alertLedState() = enabled;
}

inline RTC_TimeTypeDef& pendingRtcTime() {
  static RTC_TimeTypeDef value{};
  return value;
}

inline void setRtcTime(RTC_TimeTypeDef* time) { pendingRtcTime() = *time; }

inline void setRtcDate(RTC_DateTypeDef* date) {
  time_t currentEpoch = time(nullptr);
  struct tm parts {};
  gmtime_r(&currentEpoch, &parts);
  parts.tm_year = date->Year - 1900;
  parts.tm_mon = date->Month - 1;
  parts.tm_mday = date->Date;
  parts.tm_wday = date->WeekDay;
  parts.tm_hour = pendingRtcTime().Hours;
  parts.tm_min = pendingRtcTime().Minutes;
  parts.tm_sec = pendingRtcTime().Seconds;

  struct timeval value {};
  value.tv_sec = mktime(&parts);
  settimeofday(&value, nullptr);
}

inline void getRtcTime(RTC_TimeTypeDef* time) {
  const time_t currentEpoch = ::time(nullptr);
  struct tm parts {};
  gmtime_r(&currentEpoch, &parts);
  time->Hours = parts.tm_hour;
  time->Minutes = parts.tm_min;
  time->Seconds = parts.tm_sec;
}

inline void getRtcDate(RTC_DateTypeDef* date) {
  const time_t currentEpoch = ::time(nullptr);
  struct tm parts {};
  gmtime_r(&currentEpoch, &parts);
  date->WeekDay = parts.tm_wday;
  date->Month = parts.tm_mon + 1;
  date->Date = parts.tm_mday;
  date->Year = parts.tm_year + 1900;
}
#else
inline TFT_eSPI& lcd() { return M5.Lcd; }
inline void begin() { M5.begin(); }
inline void initImu() { M5.Imu.Init(); }
inline void getAccelData(float* ax, float* ay, float* az) { M5.Imu.getAccelData(ax, ay, az); }
inline float batteryVoltage() { return M5.Axp.GetBatVoltage(); }
inline int batteryCurrent() { return M5.Axp.GetBatCurrent(); }
inline float vbusVoltage() { return M5.Axp.GetVBusVoltage(); }
inline bool isCharging() { return batteryCurrent() > 1; }
inline uint8_t powerButtonPress() { return M5.Axp.GetBtnPress(); }
inline void powerOff() { M5.Axp.PowerOff(); }
inline float temperatureC() { return M5.Axp.GetTempInAXP192(); }
inline void screenBreath(uint8_t percent) { M5.Axp.ScreenBreath(percent); }
inline void setScreenPower(bool enabled) { M5.Axp.SetLDO2(enabled); }
inline void beginAudio() { M5.Beep.begin(); }
inline void updateAudio() { M5.Beep.update(); }
inline void tone(uint16_t frequency, uint16_t duration) { M5.Beep.tone(frequency, duration); }
inline void initAlertLed(int pin) { pinMode(pin, OUTPUT); digitalWrite(pin, HIGH); }
inline void setAlertLed(bool enabled) { digitalWrite(10, enabled ? LOW : HIGH); }
inline void setRtcTime(RTC_TimeTypeDef* time) { M5.Rtc.SetTime(time); }
inline void setRtcDate(RTC_DateTypeDef* date) { M5.Rtc.SetDate(date); }
inline void getRtcTime(RTC_TimeTypeDef* time) { M5.Rtc.GetTime(time); }
inline void getRtcDate(RTC_DateTypeDef* date) { M5.Rtc.GetDate(date); }
#endif
}  // namespace device