/*
   config.h - Hardware and signal configuration for the IRIG-B122 master clock
*/
#ifndef CONFIG_H
#define CONFIG_H

// ---- Hardware (classic ESP32 DevKit) ----
#define PIN_DAC_OUT       25      // DAC channel 1, IRIG-B AM output
#define PIN_LED           2       // Onboard LED
#define LED_ACTIVE_HIGH   1       // 1 = LED lights when pin is HIGH

// ---- I2C bus (PCF8583 and OLED display) ----
#define PIN_I2C_SDA       21
#define PIN_I2C_SCL       22
#define I2C_CLOCK_HZ      400000

// ---- PCF8583 real-time clock (optional) ----
// When present, the PCF8583 is the master clock; NTP only corrects it.
#define PIN_RTC_INT       4       // PCF8583 INT (open drain, 1 Hz), needs a pull-up
#define RTC_I2C_ADDR      0x50    // A0 to GND; 0x51 with A0 to VDD
#define RTC_SET_THRESHOLD_US 20000  // correct the PCF when it deviates more than this from NTP
#define RTC_IMMEDIATE_US  500000  // larger deviation: correct without waiting for a second check

// ---- OLED display (optional): GM009605 / SSD1306 128x64 I2C ----
#define DISPLAY_I2C_ADDR  0x3C    // some modules use 0x3D
#define DISPLAY_CONTRAST  128     // 0..255
#define DISPLAY_FLIP      0       // 1 = rotate 180 degrees

// ---- Front panel buttons (to GND, internal pull-ups) ----
// SET: held during boot = WiFi portal, long press = set date/time,
//      short press = NTP sync on/off
#define PIN_BTN_SET       5       // strapping pin, but harmless: only SDIO timing, and only when held during reset
#define PIN_BTN_UP        18
#define PIN_BTN_DOWN      19
#define BTN_LONG_MS       1000    // long press on SET
#define BTN_REPEAT_DELAY_MS 500   // UP/DOWN held: start repeating after this
#define BTN_REPEAT_MS     120     // UP/DOWN repeat interval
#define MENU_TIMEOUT_MS   60000   // leave time setting without saving after this idle time

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

// ---- Time zone defaults (changeable in the captive portal) ----
#define DEFAULT_TZ_OFFSET_MIN 60  // minutes east of UTC: 60 = UTC+1 (Netherlands)
#define DEFAULT_DST       true    // automatic European summer time

// ---- Defaults (changeable in the captive portal) ----
#define DEFAULT_NTP       "pool.ntp.org"
#define AP_NAME_PREFIX    "IRIGB_"

#endif
