/*
   Display.cpp - Optional OLED clock display (GM009605 / SSD1306 128x64, I2C)

   Runs in its own task and redraws as soon as the reference second changes.
   The time comes from TimeRef, so the display always matches the IRIG-B
   frames. Only the transfer to the display takes the shared I2C bus.

   The content shifts one pixel every 10 minutes against OLED burn-in.
*/
#include "Display.h"

#include <Arduino.h>
#include <U8g2lib.h>
#include <time.h>
#include "config.h"
#include "I2cBus.h"
#include "TimeRef.h"
#include "Menu.h"

static U8G2_SSD1306_128X64_NONAME_F_HW_I2C s_oled(DISPLAY_FLIP ? U8G2_R2 : U8G2_R0);

static volatile bool s_wifi = false;
static volatile bool s_timeValid = false;
static volatile bool s_irig = false;
static volatile bool s_portal = false;
static volatile bool s_ntpOff = false;
static volatile int  s_rssi = 0;        // dBm, 0 = not connected

static const char *const DAY_NAMES[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const MONTH_NAMES[12] = {
  "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

static int s_shift = 0;

static void drawCentered(int y, const char *s) {
  s_oled.drawStr((128 - s_oled.getStrWidth(s)) / 2 + s_shift, y, s);
}

static void drawPortal() {
  char ap[33];
  snprintf(ap, sizeof(ap), "%s%lu", AP_NAME_PREFIX, (unsigned long)(uint32_t)ESP.getEfuseMac());

  s_oled.setFont(u8g2_font_6x10_tf);
  drawCentered(10, "WiFi setup");
  drawCentered(28, "Connect to network");
  drawCentered(42, ap);
  drawCentered(60, "and open 192.168.4.1");
}

/// WiFi signal strength as 4 bars (bottom left corner at x, y);
/// not connected: empty bars with a small cross
static void drawSignal(int x, int y) {
  int bars = 0;
  if (s_wifi) {
    int rssi = s_rssi;
    bars = rssi > -55 ? 4 : rssi > -65 ? 3 : rssi > -75 ? 2 : rssi > -85 ? 1 : 0;
  }
  for (int i = 0; i < 4; i++) {
    int h = 2 + i * 2;                       // 2, 4, 6, 8 pixels high
    int bx = x + i * 3;
    if (i < bars) {
      s_oled.drawBox(bx, y - h + 1, 2, h);
    } else {
      s_oled.drawPixel(bx, y);               // empty bar: just the base
      s_oled.drawPixel(bx + 1, y);
    }
  }
  if (!s_wifi) {
    s_oled.drawLine(x + 13, y - 4, x + 17, y);
    s_oled.drawLine(x + 13, y, x + 17, y - 4);
  }
}

/// Date/time editor: the selected field is underlined
static void drawEditor(const MenuView &v) {
  char date[16], time[16];
  snprintf(date, sizeof(date), "%02d-%02d-%04d", v.values[FIELD_DAY], v.values[FIELD_MONTH], v.values[FIELD_YEAR]);
  snprintf(time, sizeof(time), "%02d:%02d:%02d", v.values[FIELD_HOUR], v.values[FIELD_MINUTE], v.values[FIELD_SECOND]);

  s_oled.setFont(u8g2_font_6x10_tf);
  drawCentered(10, "Set date/time");

  // 10x20 font: every character is 10 pixels wide
  const int dateX = (128 - 10 * 10) / 2;
  const int timeX = (128 - 8 * 10) / 2;
  s_oled.setFont(u8g2_font_10x20_tf);
  s_oled.drawStr(dateX, 34, date);
  s_oled.drawStr(timeX, 58, time);

  // Field position: first character and length within its line
  static const int FIELD_POS[FIELD_COUNT][3] = {
    // line (0 = date, 1 = time), first char, chars
    { 0, 0, 2 }, { 0, 3, 2 }, { 0, 6, 4 }, { 1, 0, 2 }, { 1, 3, 2 }, { 1, 6, 2 },
  };
  const int *f = FIELD_POS[v.field];
  int x = (f[0] == 0 ? dateX : timeX) + f[1] * 10;
  int y = f[0] == 0 ? 36 : 60;
  s_oled.drawBox(x, y, f[2] * 10, 2);
}

static void drawClock(int64_t nowUs, const MenuView &v) {
  char line[64];

  if (s_timeValid) {
    time_t now = (time_t)(nowUs / 1000000);
    struct tm tm;
    localtime_r(&now, &tm);

    s_oled.setFont(u8g2_font_6x10_tf);
    snprintf(line, sizeof(line), "%s %02d %s %04d", DAY_NAMES[tm.tm_wday],
             tm.tm_mday, MONTH_NAMES[tm.tm_mon], tm.tm_year + 1900);
    drawCentered(10, line);

    s_oled.setFont(u8g2_font_logisoso24_tn);
    snprintf(line, sizeof(line), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    drawCentered(44, line);

    if (v.toastActive) {
      s_oled.setFont(u8g2_font_6x10_tf);
      drawCentered(63, v.toast);
    } else {
      // Status: WiFi signal, NTP off, IRIG output
      drawSignal(0, 63);
      s_oled.setFont(u8g2_font_5x8_tf);
      if (s_ntpOff) s_oled.drawStr((128 - s_oled.getStrWidth("NTP off")) / 2, 63, "NTP off");
      const char *irig = s_irig ? "IRIG on" : "IRIG off";
      s_oled.drawStr(128 - s_oled.getStrWidth(irig), 63, irig);
    }
  } else {
    s_oled.setFont(u8g2_font_6x10_tf);
    drawCentered(10, "IRIG-B masterclock");

    s_oled.setFont(u8g2_font_logisoso24_tn);
    drawCentered(44, "--:--:--");

    s_oled.setFont(u8g2_font_6x10_tf);
    if (v.toastActive) {
      drawCentered(63, v.toast);
    } else if (s_ntpOff) {
      drawCentered(63, "Hold SET to set time");
    } else {
      drawCentered(63, s_wifi ? "Waiting for NTP" : "Connecting to WiFi");
    }
  }
}

static void displayTask(void *) {
  int64_t lastSec = -1;
  uint32_t lastDraw = 0;

  for (;;) {
    int64_t nowUs = timeRefNowUs();
    int64_t sec = nowUs / 1000000;

    MenuView view;
    menuGetView(view);

    // Redraw on every new second, at least twice a second for status changes,
    // and quickly while the buttons are in use
    uint32_t interval = (view.editing || view.toastActive) ? 50 : 500;
    if (sec != lastSec || millis() - lastDraw >= interval) {
      lastSec = sec;
      lastDraw = millis();

      static const int SHIFTS[4] = { 0, 1, 0, -1 };
      s_shift = SHIFTS[(millis() / 600000) % 4];

      s_oled.clearBuffer();
      if (s_portal) {
        drawPortal();
      } else if (view.editing) {
        drawEditor(view);
      } else {
        drawClock(nowUs, view);
      }

      i2cLock();
      s_oled.sendBuffer();
      i2cUnlock();
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void displayBegin() {
  i2cBegin();
  if (!i2cProbe(DISPLAY_I2C_ADDR)) {
    Serial.println("[OLED] no display found");
    return;
  }

  s_oled.setI2CAddress(DISPLAY_I2C_ADDR * 2);
  s_oled.setBusClock(I2C_CLOCK_HZ);
  i2cLock();
  s_oled.begin();
  s_oled.setContrast(DISPLAY_CONTRAST);
  i2cUnlock();

  xTaskCreatePinnedToCore(displayTask, "oled", 4096, NULL, 1, NULL, 0);
  Serial.println("[OLED] display found");
}

void displaySetStatus(bool wifi, int rssi, bool timeValid, bool irig, bool ntpOff) {
  s_wifi = wifi;
  s_rssi = rssi;
  s_ntpOff = ntpOff;
  s_timeValid = timeValid;
  s_irig = irig;
}

void displaySetPortal(bool active) {
  s_portal = active;
}
