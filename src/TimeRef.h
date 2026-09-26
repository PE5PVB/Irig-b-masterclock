/*
   TimeRef.h - Time reference for the IRIG-B output

   With a PCF8583 real-time clock present (and its 1 Hz INT output connected)
   the PCF8583 is the master clock: every second edge on INT is time-stamped
   against esp_timer and labelled with the time read from the PCF8583.
   NTP (the ESP32 system clock) is then only used to correct the PCF8583.

   Without a PCF8583 the ESP32 system clock (set by NTP) is the reference.
*/
#ifndef TIMEREF_H
#define TIMEREF_H

#include <stdint.h>

/// Detect the PCF8583 and start the RTC task
void timeRefBegin();

/// True while the PCF8583 is the reference (detected and delivering 1 Hz)
bool timeRefUsingRtc();

/// True when the PCF8583 holds a valid time
bool timeRefRtcValid();

/// Reference time (UTC epoch, microseconds) minus esp_timer_get_time()
int64_t timeRefOffsetUs();

/// Reference time now (UTC epoch, microseconds)
int64_t timeRefNowUs();

/// Compare the PCF8583 with the (NTP-synced) system clock and correct it when needed
void timeRefSyncRtc();

#endif
