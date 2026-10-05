/*
   TimeZone.cpp - Time zone setting: UTC offset plus optional European summer time
*/
#include "TimeZone.h"

// Offsets in use worldwide, in minutes east of UTC
static const int16_t OFFSETS[] = {
  -720, -660, -600, -570, -540, -480, -420, -360, -300, -240, -210, -180, -120, -60,
  0, 60, 120, 180, 210, 240, 270, 300, 330, 345, 360, 390, 420, 480, 525, 540, 570,
  600, 630, 660, 720, 765, 780, 840,
};

static const char *offsetHint(int offsetMin) {
  switch (offsetMin) {
    case 0:   return " (London, Lisbon)";
    case 60:  return " (Amsterdam, Brussels, Berlin)";
    case 120: return " (Athens, Helsinki)";
    default:  return "";
  }
}

/// "UTC-1" style name for the C library: POSIX counts the offset west of UTC
static void posixOffset(int offsetMin, char *buf, size_t len) {
  int a = abs(offsetMin);
  if (a % 60) {
    snprintf(buf, len, "UTC%c%d:%02d", offsetMin > 0 ? '-' : '+', a / 60, a % 60);
  } else {
    snprintf(buf, len, "UTC%c%d", offsetMin > 0 ? '-' : '+', a / 60);
  }
}

void timeZoneString(int offsetMin, bool dst, char *buf, size_t len) {
  char std[16];
  posixOffset(offsetMin, std, sizeof(std));
  if (!dst) {
    strlcpy(buf, std, len);
    return;
  }

  // EU rule: both changes at 01:00 UTC, expressed in local time (standard
  // time for the start, summer time for the end)
  int start = 60 + offsetMin;
  int end = 120 + offsetMin;
  if (start < 0 || end >= 24 * 60) {
    start = 120;
    end = 180;
  }
  snprintf(buf, len, "%sDST,M3.5.0/%d:%02d,M10.5.0/%d:%02d",
           std, start / 60, start % 60, end / 60, end % 60);
}

bool timeZoneValidOffset(int offsetMin) {
  for (int16_t o : OFFSETS) {
    if (o == offsetMin) return true;
  }
  return false;
}

String timeZoneOptionsHtml(int selectedOffsetMin) {
  String html;
  html.reserve(2400);
  for (int16_t o : OFFSETS) {
    char opt[96];
    int a = abs(o);
    snprintf(opt, sizeof(opt), "<option value='%d'%s>UTC%c%02d:%02d%s</option>",
             o, o == selectedOffsetMin ? " selected" : "", o < 0 ? '-' : '+',
             a / 60, a % 60, offsetHint(o));
    html += opt;
  }
  return html;
}
