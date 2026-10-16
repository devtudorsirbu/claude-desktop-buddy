#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "ble_bridge.h"
#include "xfer.h"

struct TamaState {
  uint8_t  sessionsTotal;
  uint8_t  sessionsRunning;
  uint8_t  sessionsWaiting;
  bool     recentlyCompleted;
  uint32_t tokensToday;
  uint32_t lastUpdated;
  char     msg[24];
  bool     connected;
  char     lines[8][92];
  uint8_t  nLines;
  uint16_t lineGen;          // bumps when lines change — lets UI reset scroll
  char     promptId[40];     // pending permission request ID; empty = no prompt
  char     promptTool[20];
  char     promptHint[128];
};

static size_t _utf8Width(const char* text, size_t remaining) {
  const unsigned char lead = static_cast<unsigned char>(text[0]);
  if (lead < 0x80) return 1;

  size_t width = 0;
  if (lead >= 0xC2 && lead <= 0xDF) width = 2;
  else if (lead >= 0xE0 && lead <= 0xEF) width = 3;
  else if (lead >= 0xF0 && lead <= 0xF4) width = 4;
  else return 0;
  if (width > remaining) return width;

  for (size_t index = 1; index < width; index++) {
    if ((static_cast<unsigned char>(text[index]) & 0xC0) != 0x80) return 0;
  }
  return width;
}

template<size_t N>
static void _copyUtf8(char (&destination)[N], const char* source) {
  if (!source || N == 0) {
    if (N > 0) destination[0] = '\0';
    return;
  }

  const size_t sourceLength = strlen(source);
  size_t sourceOffset = 0;
  size_t destinationOffset = 0;
  while (sourceOffset < sourceLength && destinationOffset + 1 < N) {
    const size_t width = _utf8Width(source + sourceOffset, sourceLength - sourceOffset);
    if (width == 0) {
      destination[destinationOffset++] = '?';
      sourceOffset++;
    } else if (width > sourceLength - sourceOffset || destinationOffset + width >= N) {
      break;
    } else {
      memcpy(destination + destinationOffset, source + sourceOffset, width);
      destinationOffset += width;
      sourceOffset += width;
    }
  }
  destination[destinationOffset] = '\0';
}

static size_t _utf8PrefixBytes(const char* text, size_t maxBytes) {
  const size_t length = strlen(text);
  size_t offset = 0;
  while (offset < length && offset < maxBytes) {
    const size_t width = _utf8Width(text + offset, length - offset);
    if (width == 0) { offset++; continue; }
    if (width > length - offset || offset + width > maxBytes) break;
    offset += width;
  }
  return offset;
}

// ---------------------------------------------------------------------------
// Three modes, checked in priority order:
//   demo   → auto-cycle fake scenarios every 8s, ignore live data
//   live   → JSON arrived in the last 10s over USB or BT
//   asleep → no data, all zeros, "No Claude connected"
// ---------------------------------------------------------------------------

static uint32_t _lastLiveMs = 0;
static uint32_t _lastBtByteMs = 0;   // hasClient() lies; track actual BT traffic
static bool     _demoMode   = false;
static uint8_t  _demoIdx    = 0;
static uint32_t _demoNext   = 0;

struct _Fake { const char* n; uint8_t t,r,w; bool c; uint32_t tok; };
static const _Fake _FAKES[] = {
  {"asleep",0,0,0,false,0}, {"one idle",1,0,0,false,12000},
  {"busy",4,3,0,false,89000}, {"attention",2,1,1,false,45000},
  {"completed",1,0,0,true,142000},
};

inline void dataSetDemo(bool on) {
  _demoMode = on;
  if (on) { _demoIdx = 0; _demoNext = millis(); }
}
inline bool dataDemo() { return _demoMode; }

inline bool dataConnected() {
  return _lastLiveMs != 0 && (millis() - _lastLiveMs) <= 30000;
}

inline bool dataBtActive() {
  // Desktop's idle keepalive is ~10s; give it 1.5x headroom.
  return _lastBtByteMs != 0 && (millis() - _lastBtByteMs) <= 15000;
}

inline const char* dataScenarioName() {
  if (_demoMode) return _FAKES[_demoIdx].n;
  if (dataConnected()) return dataBtActive() ? "bt" : "usb";
  return "none";
}

// Set true once the bridge sends a time sync — until then the RTC may
// hold whatever was on the coin cell (or 2000-01-01 if it lost power).
static bool _rtcValid = false;
inline bool dataRtcValid() { return _rtcValid; }

static void _applyJson(const char* line, TamaState* out) {
  JsonDocument doc;
  if (deserializeJson(doc, line)) return;
  if (xferCommand(doc)) { _lastLiveMs = millis(); return; }

  // Bridge sends {"time":[epoch_sec, tz_offset_sec]}; gmtime_r on the
  // adjusted epoch yields local components including weekday.
  JsonArray t = doc["time"];
  if (!t.isNull() && t.size() == 2) {
    time_t local = (time_t)t[0].as<uint32_t>() + (int32_t)t[1];
    struct tm lt; gmtime_r(&local, &lt);
    RTC_TimeTypeDef tm = { (uint8_t)lt.tm_hour, (uint8_t)lt.tm_min, (uint8_t)lt.tm_sec };
    RTC_DateTypeDef dt = { (uint8_t)lt.tm_wday, (uint8_t)(lt.tm_mon + 1),
                           (uint8_t)lt.tm_mday, (uint16_t)(lt.tm_year + 1900) };
    device::setRtcTime(&tm);
    device::setRtcDate(&dt);
    extern uint32_t _clkLastRead;
    _clkLastRead = 0;   // force re-read so _clkDt and _rtcValid agree
    _rtcValid = true;
    _lastLiveMs = millis();
    return;
  }

  out->sessionsTotal     = doc["total"]     | out->sessionsTotal;
  out->sessionsRunning   = doc["running"]   | out->sessionsRunning;
  out->sessionsWaiting   = doc["waiting"]   | out->sessionsWaiting;
  out->recentlyCompleted = doc["completed"] | false;
  uint32_t bridgeTokens = doc["tokens"] | 0;
  if (doc["tokens"].is<uint32_t>()) statsOnBridgeTokens(bridgeTokens);
  out->tokensToday = doc["tokens_today"] | out->tokensToday;
  const char* m = doc["msg"];
  if (m) _copyUtf8(out->msg, m);
  JsonArray la = doc["entries"];
  if (!la.isNull()) {
    uint8_t n = 0;
    for (JsonVariant v : la) {
      if (n >= 8) break;
      const char* s = v.as<const char*>();
      _copyUtf8(out->lines[n], s ? s : "");
      n++;
    }
    if (n != out->nLines || (n > 0 && strcmp(out->lines[n-1], out->msg) != 0)) {
      out->lineGen++;
    }
    out->nLines = n;
  }
  JsonObject pr = doc["prompt"];
  if (!pr.isNull()) {
    const char* pid = pr["id"]; const char* pt = pr["tool"]; const char* ph = pr["hint"];
    _copyUtf8(out->promptId, pid ? pid : "");
    _copyUtf8(out->promptTool, pt ? pt : "");
    _copyUtf8(out->promptHint, ph ? ph : "");
  } else {
    out->promptId[0] = 0; out->promptTool[0] = 0; out->promptHint[0] = 0;
  }
  out->lastUpdated = millis();
  _lastLiveMs = millis();
}

template<size_t N>
struct _LineBuf {
  char buf[N];
  size_t len = 0;
  bool overflow = false;

  void feedByte(int value, TamaState* out, const char* source) {
    if (value < 0) return;
    if (value == '\n' || value == '\r') {
      if (overflow) {
        Serial.printf("[data] %s snapshot exceeded line buffer, dropped\n", source);
        overflow = false;
        len = 0;
        return;
      }
      if (len > 0) {
        buf[len] = '\0';
        if (buf[0] == '{') _applyJson(buf, out);
        len = 0;
      }
      return;
    }

    if (overflow) return;
    if (len < N - 1) buf[len++] = static_cast<char>(value);
    else overflow = true;
  }

  void feed(Stream& stream, TamaState* out, const char* source) {
    for (size_t reads = 0; reads < 2048; reads++) {
      const int value = stream.read();
      if (value < 0) break;
      feedByte(value, out, source);
    }
  }
};

static _LineBuf<4096> _usbLine, _btLine;
static uint32_t _reportedBleDrops = 0;

inline void dataPoll(TamaState* out) {
  uint32_t now = millis();

  if (_demoMode) {
    if (now >= _demoNext) { _demoIdx = (_demoIdx + 1) % 5; _demoNext = now + 8000; }
    const _Fake& s = _FAKES[_demoIdx];
    out->sessionsTotal=s.t; out->sessionsRunning=s.r; out->sessionsWaiting=s.w;
    out->recentlyCompleted=s.c; out->tokensToday=s.tok; out->lastUpdated=now;
    out->connected = true;
    snprintf(out->msg, sizeof(out->msg), "demo: %s", s.n);
    return;
  }

  _usbLine.feed(Serial, out, "USB");
  // BLE ring buffer is drained manually since it's not a Stream.
  for (size_t reads = 0; reads < 2048 && bleAvailable(); reads++) {
    int c = bleRead();
    if (c < 0) break;
    _lastBtByteMs = millis();
    _btLine.feedByte(c, out, "BLE");
  }

  const uint32_t dropped = bleRxDropped();
  if (dropped != _reportedBleDrops) {
    Serial.printf("[ble] RX ring full, dropped %lu bytes total\n", (unsigned long)dropped);
    _reportedBleDrops = dropped;
  }

  out->connected = dataConnected();
  if (!out->connected) {
    out->sessionsTotal=0; out->sessionsRunning=0; out->sessionsWaiting=0;
    out->recentlyCompleted=false; out->lastUpdated=now;
    strncpy(out->msg, "No Claude connected", sizeof(out->msg)-1);
    out->msg[sizeof(out->msg)-1]=0;
  }
}
