/*
   Display.h - Optional OLED clock display (GM009605 / SSD1306 128x64, I2C)

   Shows the local time as sent in the IRIG-B frames, the date and status.
   Without a display on the bus nothing happens.
*/
#ifndef DISPLAY_H
#define DISPLAY_H

/// Detect the display and start the display task
void displayBegin();

/// Status shown on the display, set from the main loop
/// rssi: WiFi signal strength in dBm (only used when wifi is true)
void displaySetStatus(bool wifi, int rssi, bool timeValid, bool irig, bool ntpOff);

/// Show the WiFi setup instructions while the captive portal is active
void displaySetPortal(bool active);

#endif
