/*
   TimeRef.cpp - Time reference for the IRIG-B output (PCF8583 or system clock)

   PCF8583 notes:
   - With the alarm disabled, INT outputs a 1 Hz square wave. One edge falls on
     the seconds increment, the other half a second later. After each edge the
     hundredths register is read: below 50 means it was the second edge.
   - The PCF8583 keeps only 2 year bits. The full year is stored in its RAM,
     together with a marker that says the time was set by this firmware.
   - The PCF8583 holds UTC; the IRIG frames are converted to local time.
   - All I2C traffic runs in the RTC task.
*/
#include "TimeRef.h"

#include <Arduino.h>
#include <Wire.h>
#include <sys/time.h>
#include <time.h>
#include <esp_timer.h>
#include "config.h"

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

#define EDGE_TIMEOUT_US 3000000 // no second edge for this long: RTC failed

struct RtcTime {
  bool    valid;
  uint8_t hundredths;
  int64_t epoch;                // UTC seconds
};

static bool              s_present = false;   // PCF8583 answers on I2C
static volatile bool     s_failed = false;    // no 1 Hz on INT: fall back to system clock
static volatile bool     s_valid = false;     // PCF8583 holds a valid time
static volatile bool     s_syncRequest = false;
static int64_t           s_offsetUs = 0;      // PCF epoch us minus esp_timer us
static portMUX_TYPE      s_mux = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t     s_edgeQueue = NULL;
static int64_t           s_lastSecondEdge = 0;

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
  Wire.beginTransmission(RTC_I2C_ADDR);
  Wire.write(reg);
  Wire.write(data, len);
  return Wire.endTransmission() == 0;
}

static bool rtcRead(uint8_t reg, uint8_t *data, size_t len) {
  Wire.beginTransmission(RTC_I2C_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)RTC_I2C_ADDR, (uint8_t)len) != len) return false;
  for (size_t i = 0; i < len; i++) data[i] = Wire.read();
  return true;
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

/// Set the PCF8583 to the system clock, released exactly on a second boundary
static void setRtcFromSystem() {
  int64_t sysOffset = systemOffsetUs();
  int64_t nowUs = esp_timer_get_time() + sysOffset;
  int64_t target = nowUs / 1000000 + 2;

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

  // Release the counter on the second boundary; the start byte takes ~70 us on the bus
  int64_t releaseAt = target * 1000000 - sysOffset - 70;
  int64_t waitUs = releaseAt - esp_timer_get_time() - 20000;
  if (waitUs > 0) vTaskDelay(pdMS_TO_TICKS(waitUs / 1000));
  while (esp_timer_get_time() < releaseAt) { }
  uint8_t run = 0x00;
  rtcWrite(REG_CONTROL, &run, 1);

  xQueueReset(s_edgeQueue);          // drop edges seen while stopped
  s_lastSecondEdge = esp_timer_get_time();
  Serial.printf("[RTC] set to %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                year, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

// ---- 1 Hz edge handling ----

static void IRAM_ATTR onRtcEdge() {
  int64_t now = esp_timer_get_time();
  BaseType_t woken = pdFALSE;
  xQueueOverwriteFromISR(s_edgeQueue, &now, &woken);
  if (woken) portYIELD_FROM_ISR();
}

static void rtcTask(void *) {
  for (;;) {
    int64_t edge;
    if (xQueueReceive(s_edgeQueue, &edge, pdMS_TO_TICKS(500)) != pdTRUE) {
      if (!s_failed && esp_timer_get_time() - s_lastSecondEdge > EDGE_TIMEOUT_US) {
        s_failed = true;
        s_valid = false;
        Serial.printf("[RTC] no 1 Hz signal on GPIO%d, using NTP only\n", PIN_RTC_INT);
      }
      continue;
    }
    if (esp_timer_get_time() - edge > 300000) continue;   // too late to classify

    RtcTime t;
    if (!readRtcTime(t) || t.hundredths >= 50) continue;  // read error or half-second edge

    s_lastSecondEdge = edge;
    if (s_failed) {
      s_failed = false;
      Serial.println("[RTC] 1 Hz signal back");
    }

    if (t.valid) {
      setOffset(t.epoch * 1000000 - edge);
      if (!s_valid) {
        s_valid = true;
        Serial.println("[RTC] time valid, PCF8583 is the master clock");
      }
    } else if (s_valid) {
      s_valid = false;
      Serial.println("[RTC] time invalid");
    }

    if (s_syncRequest) {
      s_syncRequest = false;
      if (!t.valid) {
        Serial.println("[RTC] no valid time, setting from NTP");
        setRtcFromSystem();
      } else {
        int64_t dev = t.epoch * 1000000 - (edge + systemOffsetUs());
        Serial.printf("[RTC] deviation from NTP %+.1f ms\n", dev / 1000.0);
        if (dev > RTC_SET_THRESHOLD_US || dev < -RTC_SET_THRESHOLD_US) setRtcFromSystem();
      }
    }
  }
}

// ---- Public API ----

void timeRefBegin() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);
  Wire.beginTransmission(RTC_I2C_ADDR);
  s_present = Wire.endTransmission() == 0;
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
  s_lastSecondEdge = esp_timer_get_time();
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

void timeRefSyncRtc() {
  if (timeRefUsingRtc()) s_syncRequest = true;
}
