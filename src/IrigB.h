/*
   IrigB.h - IRIG-B122 generator (1 kHz AM sine on the ESP32 DAC)

   Frame content (B122, BCD time-of-year only):
     seconds, minutes, hours and day of year. Year, control functions and
     straight binary seconds are sent as 0.

   Time base is the ESP32 system clock (gettimeofday), including the TZ
   environment variable. Frames are aligned to the second boundary of the
   system clock and phase-corrected once per frame.
*/
#ifndef IRIGB_H
#define IRIGB_H

#include <stdint.h>

/// Set up DAC, sample timer and frame task. Output stays silent until irigEnable().
void irigBegin();

/// Start (true) or stop (false) the IRIG-B output. Start requires a valid system time.
void irigEnable(bool enable);

/// True while frames are being sent
bool irigRunning();

/// Phase error of the last frame start versus the system clock, in microseconds
int32_t irigLastErrorUs();

#endif
