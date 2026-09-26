/*
   StatusLed.cpp - Onboard LED status patterns, driven by a small FreeRTOS task
*/
#include "StatusLed.h"

#include <Arduino.h>
#include <sys/time.h>
#include "config.h"

static volatile LedMode s_mode = LED_OFF;

static void ledWrite(bool on) {
  digitalWrite(PIN_LED, (on == (LED_ACTIVE_HIGH != 0)) ? HIGH : LOW);
}

static void ledTask(void *) {
  for (;;) {
    uint32_t ms = millis();
    bool on = false;

    switch (s_mode) {
      case LED_OFF:        on = false; break;
      case LED_PORTAL:     on = (ms / 100) & 1; break;   // 5 Hz
      case LED_CONNECTING: on = (ms / 500) & 1; break;   // 1 Hz
      case LED_CONNECTED:  on = true; break;
      case LED_HEARTBEAT: {
        // Dark during the first 100 ms of each second, aligned with the IRIG frame start
        struct timeval tv;
        gettimeofday(&tv, NULL);
        on = tv.tv_usec >= 100000;
        break;
      }
    }

    ledWrite(on);
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void statusLedBegin() {
  pinMode(PIN_LED, OUTPUT);
  ledWrite(false);
  xTaskCreatePinnedToCore(ledTask, "led", 2048, NULL, 1, NULL, 1);
}

void statusLedSet(LedMode mode) {
  s_mode = mode;
}
