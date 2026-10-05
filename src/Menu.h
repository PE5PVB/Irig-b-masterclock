/*
   Menu.h - Front panel buttons: manual date/time setting and NTP on/off

   SET  held during boot: WiFi configuration portal (handled in the sketch)
   SET  long press   start setting date/time (local time)
        short press  editing: next field; on the last field: save
                     otherwise: NTP sync on/off
        long press   editing: cancel
   UP / DOWN         editing: change the selected field (hold to repeat)
*/
#ifndef MENU_H
#define MENU_H

#include <stdint.h>

enum MenuEvent {
  MENU_NONE,
  MENU_TOGGLE_NTP,      // short press on SET outside the editor
  MENU_SET_TIME,        // date/time confirmed: see menuSetEpoch() / menuSetPressUs()
};

enum MenuField { FIELD_DAY, FIELD_MONTH, FIELD_YEAR, FIELD_HOUR, FIELD_MINUTE, FIELD_SECOND, FIELD_COUNT };

/// What the display shows of the menu
struct MenuView {
  bool editing;
  int  field;                 // MenuField being edited
  int  values[FIELD_COUNT];   // day, month, year, hour, minute, second
  bool toastActive;
  char toast[24];             // short message shown for a moment
};

void menuBegin();

/// Poll the buttons. nowUs: reference time (UTC epoch us), timeValid: whether it is usable
MenuEvent menuLoop(int64_t nowUs, bool timeValid);

/// UTC epoch (seconds) of the confirmed date/time
int64_t menuSetEpoch();

/// esp_timer time of the press that confirmed it
int64_t menuSetPressUs();

/// Show a short message on the display for 2 seconds
void menuToast(const char *text);

/// Snapshot for the display task
void menuGetView(MenuView &view);

#endif
