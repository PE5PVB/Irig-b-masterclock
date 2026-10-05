/*
   TimeRef.cpp - Time reference for the IRIG-B output (PCF8583 or system clock)

   PCF8583 notes:
   - With the alarm disabled, INT outputs a 1 Hz square wave. Its edges do not
     fall on the seconds increment, but at a fixed, unknown point in the PCF
     second. After each edge the hundredths register is polled until it
     changes: that change happens at a known PCF time, so the PCF time of the
     edge follows from it (about 0.1 ms precision). Both edges are used.
   - The PCF8583 keeps only 2 year bits. The full year is stored in its RAM,
     together with a marker that says the time was set by this firmware.
   - The PCF8583 holds UTC; the IRIG frames are converted to local time.
   - All PCF8583 traffic runs in the RTC task, under the shared I2C bus lock.
*/
#include "TimeRef.h"

#include <Arduino.h>
#include <Wire.h>
#include <sys/time.h>
#include <time.h>
#include <esp_timer.h>
#include "config.h"
#include "I2cBus.h"

// PCF8583 registers
#define REG_CONTROL     0x00
#define REG_HUNDREDTHS  0x01
#define REG_ALARM_CTRL  0x08
#define REG_RAM         0x10    // RAM: 'I', 'B', year high, year low

#define CTRL_STOP       0x80    // stop counting, reset divider
#define CTRL_FUNCTION   0x30    // function mode (00 = 32.768 kHz clock)
#define CTRL_MASK       0x08    // mask flag (0 = year and weekday bits readable)
#define CTRL_ALARM_EN   0x04    // alarm enable (0 = 1 Hz on INT)

#define RAM_MAGIC0      'I'
#define RAM_MAGIC1      'B'

#define EDGE_TIMEOUT_US 3000000 // no edge for this long: RTC failed
#define POLL_TIMEOUT_US 15000   // hundredths must change within this time
#define OFFSET_JUMP_US  2000    // larger offset change: restart the filter

struct RtcTime {
  bool    valid;
  uint8_t hundredths;
  int64_t epoch;                // UTC seconds
};

static bool              s_present = false;   // PCF8583 answers on I2C
static volatile bool     s_failed = false;    // no 1 Hz on INT: fall back to system clock
static volatile bool     s_valid = false;     // PCF8583 holds a valid time
static volatile bool     s_syncRequest = false;
static volatile bool     s_syncTrust = false;  // correct on this check without confirmation
static int64_t           s_offsetUs = 0;      // PCF epoch us minus esp_timer us
static portMUX_TYPE      s_mux = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t     s_edgeQueue = NULL;
static int64_t           s_lastEdge = 0;
static bool              s_haveOffset = false; // filtered offset initialised
static int64_t           s_filtOffsetUs = 0;   // filtered PCF epoch us minus esp_timer us
static int               s_phaseLogs = 0;      // edge phases still to log
static int               s_lastDevDir = 0;     // direction of the last NTP deviation over the threshold
static volatile bool     s_manualRequest = false;
static int64_t           s_manualEpoch = 0;    // manually entered time (UTC seconds)...
static int64_t           s_manualPressUs = 0;  // ...valid at this esp_timer time

// ---- Helpers ----

static uint8_t bcd2bin(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
static uint8_t bin2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

/// Days since 1970-01-01 for a civil date (proleptic Gregorian)
static int64_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  int era = (y >= 0 ? y : y - 399) / 400;
  int yoe = y - era * 400;
  int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t)era * 146097 + doe - 719468;
}

static int64_t systemOffsetUs() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  int64_t mono = esp_timer_get_time();
  return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec - mono;
}

static void setOffset(int64_t us) {
  portENTER_CRITICAL(&s_mux);
  s_offsetUs = us;
  portEXIT_CRITICAL(&s_mux);
}

// ---- PCF8583 access ----

static bool rtcWrite(uint8_t reg, const uint8_t *data, size_t len) {
  i2cLock();
  Wire.beginTransmission(RTC_I2C_ADDR);
  Wire.write(reg);
  Wire.write(data, len);
  bool ok = Wire.endTransmission() == 0;
  i2cUnlock();
  return ok;
}

static bool rtcRead(uint8_t reg, uint8_t *data, size_t len) {
  bool ok = false;
  i2cLock();
  Wire.beginTransmission(RTC_I2C_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) == 0 &&
      Wire.requestFrom((uint8_t)RTC_I2C_ADDR, (uint8_t)len) == len) {
    for (size_t i = 0; i < len; i++) data[i] = Wire.read();
    ok = true;
  }
  i2cUnlock();
  return ok;
}

static bool readRtcTime(RtcTime &t) {
  uint8_t r[6], ram[4];
  t.valid = false;
  if (!rtcRead(REG_HUNDREDTHS, r, sizeof(r))) return false;
  if (!rtcRead(REG_RAM, ram, sizeof(ram))) return false;

  t.hundredths = bcd2bin(r[0]);
  int sec   = bcd2bin(r[1]);
  int min   = bcd2bin(r[2]);
  int hour  = bcd2bin(r[3] & 0x3F);
  int date  = bcd2bin(r[4] & 0x3F);
  int month = bcd2bin(r[5] & 0x1F);
  int yearBits = r[4] >> 6;

  if (ram[0] != RAM_MAGIC0 || ram[1] != RAM_MAGIC1) return true;
  int year = (ram[2] << 8) | ram[3];

  // The PCF8583 counts the year modulo 4: follow it and keep RAM up to date
  if (yearBits != (year & 3)) {
    year += (yearBits - (year & 3)) & 3;
    uint8_t y[2] = { (uint8_t)(year >> 8), (uint8_t)year };
    rtcWrite(REG_RAM + 2, y, sizeof(y));
  }

  if (year < 2024 || year > 2099 || month < 1 || month > 12 || date < 1 || date > 31 ||
      hour > 23 || min > 59 || sec > 59 || t.hundredths > 99) {
    return true;
  }

  t.epoch = daysFromCivil(year, month, date) * 86400 + hour * 3600 + min * 60 + sec;
  t.valid = true;
  return true;
}

/// Set the PCF8583 to `target` (UTC seconds), counting starts at esp_timer time `releaseAt`
static void setRtcAt(int64_t target, int64_t releaseAt) {
  // The new PCF time line is known now: switch display and IRIG over at once
  // instead of after the first edge following the release. The start byte
  // reaches the PCF8583 ~70 us after releaseAt.
  s_filtOffsetUs = target * 1000000 - (releaseAt + 70);
  s_haveOffset = true;
  setOffset(s_filtOffsetUs);
  s_valid = true;

  time_t t = (time_t)target;
  struct tm tm;
  gmtime_r(&t, &tm);
  int year = tm.tm_year + 1900;

  uint8_t regs[7] = {
    CTRL_STOP,                                           // control: stopped, divider reset
    0x00,                                                // hundredths
    bin2bcd(tm.tm_sec),
    bin2bcd(tm.tm_min),
    bin2bcd(tm.tm_hour),                                 // 24 h format
    (uint8_t)(((year & 3) << 6) | bin2bcd(tm.tm_mday)),
    (uint8_t)((tm.tm_wday << 5) | bin2bcd(tm.tm_mon + 1)),
  };
  uint8_t alarmCtrl = 0x00;
  uint8_t ram[4] = { RAM_MAGIC0, RAM_MAGIC1, (uint8_t)(year >> 8), (uint8_t)year };

  if (!rtcWrite(REG_CONTROL, regs, sizeof(regs)) ||
      !rtcWrite(REG_ALARM_CTRL, &alarmCtrl, 1) ||
      !rtcWrite(REG_RAM, ram, sizeof(ram))) {
    Serial.println("[RTC] write failed");
    return;
  }

  // Release the counter at releaseAt. Take the bus well before, so a display
  // update cannot delay the release.
  int64_t waitUs = releaseAt - esp_timer_get_time() - 100000;
  if (waitUs > 0) vTaskDelay(pdMS_TO_TICKS(waitUs / 1000));
  i2cLock();
  int64_t lateUs = esp_timer_get_time() - releaseAt;
  while (esp_timer_get_time() < releaseAt) { }
  uint8_t run = 0x00;
  rtcWrite(REG_CONTROL, &run, 1);
  i2cUnlock();
  if (lateUs > 0) Serial.printf("[RTC] released %lld us late\n", (long long)lateUs);

  xQueueReset(s_edgeQueue);          // drop edges seen while stopped
  s_lastEdge = esp_timer_get_time();
  s_phaseLogs = 2;
  s_lastDevDir = 0;
  Serial.printf("[RTC] set to %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                year, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/// Set the PCF8583 to the system clock, released exactly on a second boundary
static void setRtcFromSystem() {
  int64_t sysOffset = systemOffsetUs();
  int64_t target = (esp_timer_get_time() + sysOffset) / 1000000 + 2;
  // The start byte takes ~70 us on the bus
  setRtcAt(target, target * 1000000 - sysOffset - 70);
}

/// Set the PCF8583 to the manually entered time. That time was valid at the
/// press; release a whole number of seconds later so the PCF8583 starts on
/// the same second grid.
static void setRtcManual(int64_t epoch, int64_t pressUs) {
  int64_t k = 1;
  while (pressUs + k * 1000000 - esp_timer_get_time() < 150000) k++;
  setRtcAt(epoch + k, pressUs + k * 1000000 - 70);
}

// ---- 1 Hz edge handling ----

static void IRAM_ATTR onRtcEdge() {
  int64_t now = esp_timer_get_time();
  BaseType_t woken = pdFALSE;
  xQueueOverwriteFromISR(s_edgeQueue, &now, &woken);
  if (woken) portYIELD_FROM_ISR();
}

/// PCF time (UTC epoch us) at an edge: poll the hundredths register until it
/// changes. The change happens on a whole hundredth of PCF time, so the edge
/// lies (change time - edge time) before that.
static bool edgePcfTime(const RtcTime &t, int64_t edge, int64_t &pcfUs) {
  bool ok = false;
  i2cLock();                         // keep the bus free of display traffic while polling
  uint8_t r;
  if (!rtcRead(REG_HUNDREDTHS, &r, 1)) {
    i2cUnlock();
    return false;
  }
  int64_t start = esp_timer_get_time();
  int64_t prevEnd = start;
  uint8_t h0 = bcd2bin(r);
  int64_t epoch = t.epoch + (h0 < t.hundredths ? 1 : 0);   // second rolled over since readRtcTime
  for (;;) {
    if (!rtcRead(REG_HUNDREDTHS, &r, 1)) break;
    int64_t end = esp_timer_get_time();
    uint8_t h = bcd2bin(r);
    if (h != h0) {
      if ((h + 100 - h0) % 100 == 1) {
        int64_t changeAt = (prevEnd + end) / 2;
        pcfUs = epoch * 1000000 + (int64_t)(h0 + 1) * 10000 - (changeAt - edge);
        ok = true;
      }
      break;
    }
    prevEnd = end;
    if (end - start > POLL_TIMEOUT_US) break;
  }
  i2cUnlock();
  return ok;
}

static void rtcTask(void *) {
  for (;;) {
    if (s_manualRequest) {
      portENTER_CRITICAL(&s_mux);
      int64_t epoch = s_manualEpoch;
      int64_t pressUs = s_manualPressUs;
      s_manualRequest = false;
      portEXIT_CRITICAL(&s_mux);
      setRtcManual(epoch, pressUs);
    }

    int64_t edge;
    if (xQueueReceive(s_edgeQueue, &edge, pdMS_TO_TICKS(50)) != pdTRUE) {
      if (!s_failed && esp_timer_get_time() - s_lastEdge > EDGE_TIMEOUT_US) {
        s_failed = true;
        s_valid = false;
        Serial.printf("[RTC] no 1 Hz signal on GPIO%d, using NTP only\n", PIN_RTC_INT);
      }
      continue;
    }
    if (esp_timer_get_time() - edge > 300000) continue;   // too late to use
    if (s_manualRequest) continue;                          // PCF is about to be set

    RtcTime t;
    if (!readRtcTime(t)) continue;

    s_lastEdge = edge;
    if (s_failed) {
      s_failed = false;
      Serial.println("[RTC] 1 Hz signal back");
    }

    if (!t.valid) {
      if (s_valid) {
        s_valid = false;
        Serial.println("[RTC] time invalid");
      }
      if (s_syncRequest) {
        s_syncRequest = false;
        Serial.println("[RTC] no valid time, setting from NTP");
        setRtcFromSystem();
      }
      continue;
    }

    int64_t pcfUs;
    if (!edgePcfTime(t, edge, pcfUs)) continue;

    // Light filtering against I2C timing jitter; restart after a jump (PCF set)
    int64_t sample = pcfUs - edge;
    if (!s_haveOffset || llabs(sample - s_filtOffsetUs) > OFFSET_JUMP_US) {
      s_filtOffsetUs = sample;
      s_haveOffset = true;
    } else {
      s_filtOffsetUs += (sample - s_filtOffsetUs) / 8;
    }
    // Publish, unless a manual time arrived meanwhile (it already published its own)
    portENTER_CRITICAL(&s_mux);
    bool pending = s_manualRequest;
    if (!pending) s_offsetUs = s_filtOffsetUs;
    portEXIT_CRITICAL(&s_mux);
    if (pending) continue;

    if (s_phaseLogs > 0) {
      s_phaseLogs--;
      Serial.printf("[RTC] 1 Hz edge at %.1f ms in the PCF second\n", (pcfUs % 1000000) / 1000.0);
    }

    if (!s_valid) {
      s_valid = true;
      Serial.println("[RTC] time valid, PCF8583 is the master clock");
    }

    if (s_syncRequest) {
      s_syncRequest = false;
      // Normally correct only when two NTP checks in a row exceed the threshold
      // in the same direction, so a single bad NTP sample cannot move the master
      // clock. Correct at once right after NTP was switched on, or when the
      // deviation is far larger than an NTP error can be.
      int64_t dev = s_filtOffsetUs - systemOffsetUs();
      int dir = dev > RTC_SET_THRESHOLD_US ? 1 : (dev < -RTC_SET_THRESHOLD_US ? -1 : 0);
      bool now = s_syncTrust || llabs(dev) > RTC_IMMEDIATE_US;
      s_syncTrust = false;
      if (dir != 0 && (now || dir == s_lastDevDir)) {
        Serial.printf("[RTC] deviation from NTP %+.1f ms: correcting\n", dev / 1000.0);
        setRtcFromSystem();
        dir = 0;
      } else if (dir != 0) {
        Serial.printf("[RTC] deviation from NTP %+.1f ms, waiting for confirmation\n", dev / 1000.0);
      } else {
        Serial.printf("[RTC] deviation from NTP %+.1f ms\n", dev / 1000.0);
      }
      s_lastDevDir = dir;
    }
  }
}

// ---- Public API ----

void timeRefBegin() {
  i2cBegin();
  s_present = i2cProbe(RTC_I2C_ADDR);
  if (!s_present) {
    Serial.println("[RTC] no PCF8583 found, using NTP");
    return;
  }

  // Clock mode, counting, unmasked, alarm off (1 Hz on INT)
  uint8_t ctrl;
  if (rtcRead(REG_CONTROL, &ctrl, 1) && (ctrl & (CTRL_STOP | CTRL_FUNCTION | CTRL_MASK | CTRL_ALARM_EN))) {
    ctrl &= ~(CTRL_STOP | CTRL_FUNCTION | CTRL_MASK | CTRL_ALARM_EN);
    rtcWrite(REG_CONTROL, &ctrl, 1);
  }

  RtcTime t;
  if (readRtcTime(t) && t.valid) {
    time_t e = (time_t)t.epoch;
    struct tm tm;
    gmtime_r(&e, &tm);
    Serial.printf("[RTC] PCF8583 found, time %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
  } else {
    Serial.println("[RTC] PCF8583 found, no valid time: waiting for NTP");
  }

  s_edgeQueue = xQueueCreate(1, sizeof(int64_t));
  s_lastEdge = esp_timer_get_time();
  s_phaseLogs = 2;
  xTaskCreatePinnedToCore(rtcTask, "rtc", 4096, NULL, 5, NULL, 1);

  pinMode(PIN_RTC_INT, INPUT_PULLUP);
  attachInterrupt(PIN_RTC_INT, onRtcEdge, CHANGE);
}

bool timeRefUsingRtc() {
  return s_present && !s_failed;
}

bool timeRefRtcValid() {
  return timeRefUsingRtc() && s_valid;
}

int64_t timeRefOffsetUs() {
  if (!timeRefRtcValid()) return systemOffsetUs();
  portENTER_CRITICAL(&s_mux);
  int64_t us = s_offsetUs;
  portEXIT_CRITICAL(&s_mux);
  return us;
}

int64_t timeRefNowUs() {
  return esp_timer_get_time() + timeRefOffsetUs();
}

void timeRefSyncRtc(bool trust) {
  if (!timeRefUsingRtc()) return;
  if (trust) s_syncTrust = true;
  s_syncRequest = true;
}

void timeRefSetRtc(int64_t epoch, int64_t pressUs) {
  if (!timeRefUsingRtc()) return;
  portENTER_CRITICAL(&s_mux);
  s_manualEpoch = epoch;
  s_manualPressUs = pressUs;
  s_manualRequest = true;
  s_offsetUs = epoch * 1000000 - pressUs;   // show the new time at once
  portEXIT_CRITICAL(&s_mux);
  s_valid = true;
}
