/*
   IrigB.cpp - IRIG-B122 generator (1 kHz AM sine on the ESP32 DAC)

   A hardware timer outputs one DAC sample every 1/IRIG_SAMPLE_RATE seconds.
   Each 10 ms bit holds 10 carrier periods and starts on a positive-going zero
   crossing, so the carrier is phase-coherent with the bit clock.

   The timer ISR marks every frame start with esp_timer_get_time() and wakes
   the frame task. That task compares the stamp with the time reference
   (PCF8583 or system clock, see TimeRef) and
   sets a correction of whole samples, which the ISR applies at bit
   boundaries (at most one sample per bit): drop the zero-crossing sample to
   shorten, or hold it one extra sample to lengthen. It then builds the
   next frame in the back buffer.
*/
#include "IrigB.h"

#include <Arduino.h>
#include <sys/time.h>
#include <time.h>
#include <math.h>
#include <esp_timer.h>
#include "hal/dac_ll.h"
#include "config.h"
#include "TimeRef.h"

static_assert(1000000 % IRIG_SAMPLE_RATE == 0, "IRIG_SAMPLE_RATE must divide 1 MHz");
static_assert(IRIG_SAMPLE_RATE % IRIG_CARRIER_HZ == 0, "IRIG_SAMPLE_RATE must be a multiple of the carrier");
static_assert(PIN_DAC_OUT == 25 || PIN_DAC_OUT == 26, "DAC is only available on GPIO25 and GPIO26");

#define BITS_PER_FRAME     100
#define SAMPLES_PER_PERIOD (IRIG_SAMPLE_RATE / IRIG_CARRIER_HZ)
#define SAMPLES_PER_BIT    (IRIG_SAMPLE_RATE / 100)
#define SAMPLES_PER_MS     (IRIG_SAMPLE_RATE / 1000)
#define US_PER_SAMPLE      (1000000 / IRIG_SAMPLE_RATE)
#define DAC_MID            128

static const dac_channel_t DAC_CHAN = (PIN_DAC_OUT == 25) ? DAC_CHAN_0 : DAC_CHAN_1;

// ---- Shared state between ISR and frame task ----

static DRAM_ATTR uint8_t  s_markTbl[SAMPLES_PER_PERIOD];
static DRAM_ATTR uint8_t  s_spaceTbl[SAMPLES_PER_PERIOD];
static DRAM_ATTR uint16_t s_frame[2][BITS_PER_FRAME];   // high time per bit, in samples
static int64_t            s_frameSec[2];                // epoch second each buffer encodes

static volatile uint8_t  s_active = 0;       // buffer the ISR is sending
static volatile bool     s_nextReady = false;
static volatile bool     s_running = false;
static volatile int64_t  s_startAtUs = 0;    // esp_timer time to start the first frame, 0 = none
static volatile int64_t  s_frameStampUs = 0; // esp_timer time of the last frame start
static volatile int32_t  s_adjust = 0;       // >0: samples to drop, <0: samples to insert

static volatile bool     s_enabled = false;
static volatile int32_t  s_lastErrUs = 0;

static TaskHandle_t s_task = NULL;
static hw_timer_t  *s_timer = NULL;

// s_startAtUs is 64 bit: guard it so the ISR never sees a half-written value
static portMUX_TYPE s_startMux = portMUX_INITIALIZER_UNLOCKED;

static void setStartAt(int64_t us) {
  portENTER_CRITICAL(&s_startMux);
  s_startAtUs = us;
  portEXIT_CRITICAL(&s_startMux);
}

static int64_t getStartAt() {
  portENTER_CRITICAL(&s_startMux);
  int64_t us = s_startAtUs;
  portEXIT_CRITICAL(&s_startMux);
  return us;
}

// ---- ISR state ----
static uint8_t  s_bit = 0;
static uint16_t s_sample = 0;
static uint16_t s_high = 0;
static bool     s_holding = false;

static inline void IRAM_ATTR dacOut(uint8_t v) {
  dac_ll_update_output_value(DAC_CHAN, v);
}

static void IRAM_ATTR onSample() {
  if (!s_running) {
    dacOut(DAC_MID);
    portENTER_CRITICAL_ISR(&s_startMux);
    int64_t startAt = s_startAtUs;
    bool due = startAt != 0 && esp_timer_get_time() >= startAt;
    if (due) s_startAtUs = 0;
    portEXIT_CRITICAL_ISR(&s_startMux);
    if (!due) return;
    s_bit = 0;
    s_sample = 0;
    s_holding = false;
    s_running = true;
  }

  if (s_sample == 0) {
    if (s_bit == 0 && !s_holding) {
      if (s_nextReady) {
        s_active ^= 1;
        s_nextReady = false;
      }
      s_frameStampUs = esp_timer_get_time();
      BaseType_t woken = pdFALSE;
      vTaskNotifyGiveFromISR(s_task, &woken);
      if (woken) portYIELD_FROM_ISR();
    }

    if (s_adjust > 0) {
      s_adjust = s_adjust - 1;
      s_sample = 1;                       // drop the zero-crossing sample
    } else if (s_adjust < 0 && !s_holding) {
      s_adjust = s_adjust + 1;
      s_holding = true;                   // hold the zero crossing one sample longer
      dacOut(DAC_MID);
      return;
    }
    s_holding = false;
    s_high = s_frame[s_active][s_bit];
  }

  uint16_t phase = s_sample % SAMPLES_PER_PERIOD;
  dacOut(s_sample < s_high ? s_markTbl[phase] : s_spaceTbl[phase]);

  if (++s_sample >= SAMPLES_PER_BIT) {
    s_sample = 0;
    if (++s_bit >= BITS_PER_FRAME) s_bit = 0;
  }
}

// ---- Frame building ----

static void putBcd(uint16_t *f, uint8_t firstBit, uint8_t nbits, uint8_t value) {
  for (uint8_t i = 0; i < nbits; i++) {
    f[firstBit + i] = ((value >> i) & 1) ? IRIG_MS_ONE * SAMPLES_PER_MS : IRIG_MS_ZERO * SAMPLES_PER_MS;
  }
}

static void buildFrame(uint8_t buf, int64_t epochSec) {
  uint16_t *f = s_frame[buf];

  for (uint8_t i = 0; i < BITS_PER_FRAME; i++) f[i] = IRIG_MS_ZERO * SAMPLES_PER_MS;
  f[0] = IRIG_MS_MARKER * SAMPLES_PER_MS;                              // Pr
  for (uint8_t i = 9; i < BITS_PER_FRAME; i += 10) f[i] = IRIG_MS_MARKER * SAMPLES_PER_MS;  // P1..P0

  time_t t = (time_t)epochSec;
  struct tm tm;
  localtime_r(&t, &tm);
  int day = tm.tm_yday + 1;

  putBcd(f, 1, 4, tm.tm_sec % 10);
  putBcd(f, 6, 3, tm.tm_sec / 10);
  putBcd(f, 10, 4, tm.tm_min % 10);
  putBcd(f, 15, 3, tm.tm_min / 10);
  putBcd(f, 20, 4, tm.tm_hour % 10);
  putBcd(f, 25, 2, tm.tm_hour / 10);
  putBcd(f, 30, 4, day % 10);
  putBcd(f, 35, 4, (day / 10) % 10);
  putBcd(f, 40, 2, day / 100);

  s_frameSec[buf] = epochSec;
}

static void scheduleStart() {
  int64_t offset = timeRefOffsetUs();
  int64_t nowUs = esp_timer_get_time() + offset;
  int64_t sec = nowUs / 1000000 + 1;
  if (sec * 1000000 - nowUs < 100000) sec++;   // leave time to prepare

  uint8_t back = s_active ^ 1;
  buildFrame(back, sec);
  s_adjust = 0;
  s_nextReady = true;
  setStartAt(sec * 1000000 - offset);
  Serial.printf("[IRIG] start at second %lld\n", (long long)sec);
}

static void frameTask(void *) {
  for (;;) {
    if (!s_enabled) {
      s_running = false;
      setStartAt(0);
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    if (!s_running && getStartAt() == 0) scheduleStart();

    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000)) == 0) {
      if (s_running) {
        Serial.println("[IRIG] no frame start seen, restarting");
        s_running = false;
      }
      setStartAt(0);
      continue;
    }

    // Phase error of the frame that just started
    int64_t frameUs = s_frameStampUs + timeRefOffsetUs();
    int64_t sec = (frameUs + 500000) / 1000000;
    int64_t err = frameUs - sec * 1000000;
    s_lastErrUs = (int32_t)err;

    if (sec != s_frameSec[s_active] || err > IRIG_RESYNC_US || err < -IRIG_RESYNC_US) {
      Serial.printf("[IRIG] resync (frame %lld, clock %lld, error %lld us)\n",
                    (long long)s_frameSec[s_active], (long long)sec, (long long)err);
      s_running = false;
      scheduleStart();
      continue;
    }

    int32_t adj = (int32_t)((err + (err >= 0 ? US_PER_SAMPLE / 2 : -US_PER_SAMPLE / 2)) / US_PER_SAMPLE);
    if (adj > BITS_PER_FRAME - 1) adj = BITS_PER_FRAME - 1;
    if (adj < -(BITS_PER_FRAME - 1)) adj = -(BITS_PER_FRAME - 1);
    s_adjust = adj;

    buildFrame(s_active ^ 1, sec + 1);
    s_nextReady = true;
  }
}

// ---- Public API ----

void irigBegin() {
  float spaceAmp = IRIG_AMP_MARK / IRIG_MARK_SPACE;
  for (int i = 0; i < SAMPLES_PER_PERIOD; i++) {
    float s = sinf(2.0f * (float)M_PI * i / SAMPLES_PER_PERIOD);
    s_markTbl[i]  = (uint8_t)lroundf(DAC_MID + IRIG_AMP_MARK * s);
    s_spaceTbl[i] = (uint8_t)lroundf(DAC_MID + spaceAmp * s);
  }
  buildFrame(0, 0);
  buildFrame(1, 0);

  dacWrite(PIN_DAC_OUT, DAC_MID);   // powers up the DAC pad

  xTaskCreatePinnedToCore(frameTask, "irig", 4096, NULL, configMAX_PRIORITIES - 2, &s_task, 1);

  s_timer = timerBegin(1000000);
  timerAttachInterrupt(s_timer, &onSample);
  timerAlarm(s_timer, US_PER_SAMPLE, true, 0);
}

void irigEnable(bool enable) {
  s_enabled = enable;
}

bool irigRunning() {
  return s_running;
}

int32_t irigLastErrorUs() {
  return s_lastErrUs;
}
