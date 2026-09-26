/*
   StatusLed.h - Onboard LED status patterns
*/
#ifndef STATUSLED_H
#define STATUSLED_H

enum LedMode {
  LED_OFF,
  LED_PORTAL,       // fast blink: captive portal active
  LED_CONNECTING,   // slow blink: connecting to WiFi
  LED_CONNECTED,    // steady on: WiFi connected, waiting for NTP
  LED_HEARTBEAT     // on, short off at the start of every second: IRIG-B output running
};

void statusLedBegin();
void statusLedSet(LedMode mode);

#endif
