/*
   config.h - Hardware and signal configuration for the IRIG-B122 master clock
*/
#ifndef CONFIG_H
#define CONFIG_H

// ---- Hardware (classic ESP32 DevKit) ----
#define PIN_DAC_OUT       25      // DAC channel 1, IRIG-B AM output
#define PIN_LED           2       // Onboard LED
#define LED_ACTIVE_HIGH   1       // 1 = LED lights when pin is HIGH
#define PIN_BUTTON        0       // BOOT button, active LOW

// ---- IRIG-B pulse widths (high time within a 10 ms bit) ----
// IRIG 200 standard: 0 = 2 ms, 1 = 5 ms, marker = 8 ms.
// Swap IRIG_MS_ZERO and IRIG_MS_ONE for inverted bit coding.
#define IRIG_MS_ZERO      2
#define IRIG_MS_ONE       5
#define IRIG_MS_MARKER    8

// ---- AM carrier ----
#define IRIG_CARRIER_HZ   1000    // B12x: 1 kHz sine
#define IRIG_SAMPLE_RATE  40000   // DAC update rate, must divide 1 MHz and be a multiple of the carrier
#define IRIG_AMP_MARK     120     // Peak amplitude of the high part, in DAC steps (max 127)
#define IRIG_MARK_SPACE   3.0f    // Modulation ratio mark:space (IRIG allows 3:1 to 6:1)

// ---- Synchronisation ----
#define IRIG_RESYNC_US    100000  // Phase error above this restarts output on the next second
#define NTP_INTERVAL_MS   300000  // SNTP poll interval; keeps crystal drift steps small (~6 ms)

// ---- Time zone: Dutch time, CET/CEST with automatic summer/winter time ----
#define TIMEZONE          "CET-1CEST,M3.5.0,M10.5.0/3"

// ---- Defaults (changeable in the captive portal) ----
#define DEFAULT_NTP       "pool.ntp.org"
#define AP_NAME_PREFIX    "IRIGB_"

#endif
