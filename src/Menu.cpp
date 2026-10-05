/*
   Menu.cpp - Front panel buttons: manual date/time setting and NTP on/off

   Polled from the main loop. The display task reads a snapshot through
   menuGetView().
*/
#include "Menu.h"

#include <Arduino.h>
#include <time.h>
#include <esp_timer.h>
#include "config.h"

#define DEBOUNCE_MS  30
#define TOAST_MS     2000

struct Button {
  uint8_t  pin;
  bool     down;          // debounced state
  bool     raw;
  uint32_t rawChangedAt;
  uint32_t pressedAt;
  bool     longFired;
  uint32_t nextRepeat;
};

static Button s_set  = { PIN_BTN_SET,  false, false, 0, 0, false, 0 };
static Button s_up   = { PIN_BTN_UP,   false, false, 0, 0, false, 0 };
static Button s_down = { PIN_BTN_DOWN, false, false, 0, 0, false, 0 };

static MenuView     s_view = {};
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t     s_toastUntil = 0;
static uint32_t     s_lastActivity = 0;
static int64_t      s_setEpoch = 0;
static int64_t      s_setPressUs = 0;

// ---- Buttons ----

/// Debounce; returns true on the press edge
static bool updateButton(Button &b, uint32_t now) {
  bool raw = digitalRead(b.pin) == LOW;
  if (raw != b.raw) {
    b.raw = raw;
    b.rawChangedAt = now;
  }
  if (b.raw != b.down && now - b.rawChangedAt >= DEBOUNCE_MS) {
    b.down = b.raw;
    if (b.down) {
      b.pressedAt = now;
      b.longFired = false;
      b.nextRepeat = now + BTN_REPEAT_DELAY_MS;
      return true;
    }
  }
  return false;
}

/// UP/DOWN: true on the press and then repeatedly while held
static bool stepEvent(Button &b, uint32_t now) {
  if (updateButton(b, now)) return true;
  if (b.down && (int32_t)(now - b.nextRepeat) >= 0) {
    b.nextRepeat = now + BTN_REPEAT_MS;
    return true;
  }
  return false;
}

// ---- Date/time fields ----

static int daysInMonth(int year, int month) {
  static const int DAYS[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
  bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  return (month == 2 && leap) ? 29 : DAYS[month - 1];
}

static void fieldRange(const int *v, int field, int &lo, int &hi) {
  switch (field) {
    case FIELD_DAY:    lo = 1;    hi = daysInMonth(v[FIELD_YEAR], v[FIELD_MONTH]); break;
    case FIELD_MONTH:  lo = 1;    hi = 12; break;
    case FIELD_YEAR:   lo = 2024; hi = 2099; break;
    case FIELD_HOUR:   lo = 0;    hi = 23; break;
    default:           lo = 0;    hi = 59; break;   // minute, second
  }
}

static void stepField(int *v, int field, int delta) {
  int lo, hi;
  fieldRange(v, field, lo, hi);
  v[field] += delta;
  if (v[field] > hi) v[field] = lo;
  if (v[field] < lo) v[field] = hi;
  // Keep the day valid after a month or year change
  int dim = daysInMonth(v[FIELD_YEAR], v[FIELD_MONTH]);
  if (v[FIELD_DAY] > dim) v[FIELD_DAY] = dim;
}

static void startEditing(int64_t nowUs, bool timeValid) {
  struct tm tm;
  if (timeValid) {
    time_t now = (time_t)(nowUs / 1000000);
    localtime_r(&now, &tm);
  } else {
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = atoi(__DATE__ + 7) - 1900;   // build year
    tm.tm_mday = 1;
  }
  portENTER_CRITICAL(&s_mux);
  s_view.editing = true;
  s_view.field = FIELD_DAY;
  s_view.values[FIELD_DAY] = tm.tm_mday;
  s_view.values[FIELD_MONTH] = tm.tm_mon + 1;
  s_view.values[FIELD_YEAR] = tm.tm_year + 1900;
  s_view.values[FIELD_HOUR] = tm.tm_hour;
  s_view.values[FIELD_MINUTE] = tm.tm_min;
  s_view.values[FIELD_SECOND] = tm.tm_sec;
  if (s_view.values[FIELD_YEAR] < 2024) s_view.values[FIELD_YEAR] = 2024;
  portEXIT_CRITICAL(&s_mux);
  Serial.println("[MENU] set date/time");
}

static void stopEditing() {
  portENTER_CRITICAL(&s_mux);
  s_view.editing = false;
  portEXIT_CRITICAL(&s_mux);
}

// ---- Public API ----

void menuBegin() {
  pinMode(PIN_BTN_SET, INPUT_PULLUP);
  pinMode(PIN_BTN_UP, INPUT_PULLUP);
  pinMode(PIN_BTN_DOWN, INPUT_PULLUP);
}

MenuEvent menuLoop(int64_t nowUs, bool timeValid) {
  uint32_t now = millis();
  MenuEvent event = MENU_NONE;

  if (s_view.toastActive && (int32_t)(now - s_toastUntil) >= 0) {
    portENTER_CRITICAL(&s_mux);
    s_view.toastActive = false;
    portEXIT_CRITICAL(&s_mux);
  }

  // SET: short press on release, long press as soon as it is held long enough
  bool wasDown = s_set.down;
  updateButton(s_set, now);
  bool setShort = wasDown && !s_set.down && !s_set.longFired;
  bool setLong = false;
  if (s_set.down && !s_set.longFired && now - s_set.pressedAt >= BTN_LONG_MS) {
    s_set.longFired = true;
    setLong = true;
  }
  bool up = stepEvent(s_up, now);
  bool down = stepEvent(s_down, now);

  if (setShort || setLong || up || down) s_lastActivity = now;

  if (!s_view.editing) {
    if (setLong) {
      startEditing(nowUs, timeValid);
    } else if (setShort) {
      event = MENU_TOGGLE_NTP;
    }
    return event;
  }

  // Editing
  if (setLong) {
    stopEditing();
    menuToast("Cancelled");
    Serial.println("[MENU] cancelled");
    return MENU_NONE;
  }
  if (now - s_lastActivity > MENU_TIMEOUT_MS) {
    stopEditing();
    menuToast("Timeout, not saved");
    Serial.println("[MENU] timeout");
    return MENU_NONE;
  }

  portENTER_CRITICAL(&s_mux);
  if (up) stepField(s_view.values, s_view.field, +1);
  if (down) stepField(s_view.values, s_view.field, -1);
  bool save = false;
  if (setShort) {
    if (s_view.field == FIELD_SECOND) {
      save = true;
    } else {
      s_view.field++;
    }
  }
  int v[FIELD_COUNT];
  memcpy(v, s_view.values, sizeof(v));
  portEXIT_CRITICAL(&s_mux);

  if (save) {
    s_setPressUs = esp_timer_get_time();
    struct tm tm = {};
    tm.tm_mday = v[FIELD_DAY];
    tm.tm_mon = v[FIELD_MONTH] - 1;
    tm.tm_year = v[FIELD_YEAR] - 1900;
    tm.tm_hour = v[FIELD_HOUR];
    tm.tm_min = v[FIELD_MINUTE];
    tm.tm_sec = v[FIELD_SECOND];
    tm.tm_isdst = -1;                     // let the time zone rules decide
    s_setEpoch = (int64_t)mktime(&tm);    // local time -> UTC epoch
    stopEditing();
    Serial.printf("[MENU] time set to %02d-%02d-%04d %02d:%02d:%02d local\n",
                  v[FIELD_DAY], v[FIELD_MONTH], v[FIELD_YEAR], v[FIELD_HOUR], v[FIELD_MINUTE], v[FIELD_SECOND]);
    event = MENU_SET_TIME;
  }
  return event;
}

int64_t menuSetEpoch() {
  return s_setEpoch;
}

int64_t menuSetPressUs() {
  return s_setPressUs;
}

void menuToast(const char *text) {
  portENTER_CRITICAL(&s_mux);
  strlcpy(s_view.toast, text, sizeof(s_view.toast));
  s_view.toastActive = true;
  portEXIT_CRITICAL(&s_mux);
  s_toastUntil = millis() + TOAST_MS;
}

void menuGetView(MenuView &view) {
  portENTER_CRITICAL(&s_mux);
  view = s_view;
  portEXIT_CRITICAL(&s_mux);
}
