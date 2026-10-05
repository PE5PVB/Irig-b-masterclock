/*
   TimeZone.h - Time zone setting: UTC offset plus optional European summer time

   The setting is turned into a POSIX TZ string for the C library, so
   localtime_r() (IRIG frames, display) gives the local time.
*/
#ifndef TIMEZONE_H
#define TIMEZONE_H

#include <Arduino.h>

/// POSIX TZ string for an offset (minutes east of UTC) with or without
/// European summer time (last Sunday of March to last Sunday of October,
/// switching at 01:00 UTC)
void timeZoneString(int offsetMin, bool dst, char *buf, size_t len);

/// True when the offset is one of the offsets offered in the portal
bool timeZoneValidOffset(int offsetMin);

/// HTML <option> list for the portal, with the given offset selected
String timeZoneOptionsHtml(int selectedOffsetMin);

#endif
