# Irig-b masterclock

IRIG-B masterclock based on an ESP32. The clock gets the time from NTP over WiFi and transmits it continuously as **IRIG-B122**: a 1 kHz sine, amplitude-modulated with the time code. Equipment with an IRIG-B input (time code readers, recorders, measurement systems, clocks) can synchronise to it.

The transmitted time is **local time**: time zone and automatic summer time are set in the web interface. The default is Dutch time (UTC+1 with summer time).

## How it works

### IRIG-B122 in short

Every second one frame of 100 bits is sent; each bit lasts 10 ms. A bit is a piece of 1 kHz sine (10 periods) whose first part is sent at high amplitude (mark) and the rest at low amplitude (space). The length of the high part sets the meaning:

| Symbol | High part | Use |
|---|---|---|
| 0 | 2 ms | data bit 0 |
| 1 | 5 ms | data bit 1 |
| Marker | 8 ms | frame start (Pr, bit 0) and separator after each field (bit 9, 19, …, 99) |

```
 bit:   |<------------- 10 ms ------------->|
 "0":   |MMMM|ssssssssssssssssssssssssssssss|     M = mark  (high amplitude)
 "1":   |MMMMMMMMMM|ssssssssssssssssssssssss|     s = space (low amplitude)
 marker:|MMMMMMMMMMMMMMMM|ssssssssssssssssss|
```

Mark and space have a 3:1 ratio. A frame starts with two consecutive markers (P0 of the previous frame and Pr). The leading edge of Pr falls exactly on the second boundary: that is the moment the frame refers to.

Frame content (B122 = BCD time-of-year only):

| Bits | Content |
|---|---|
| 1–8 | seconds (BCD) |
| 10–17 | minutes (BCD) |
| 20–26 | hours (BCD) |
| 30–41 | day of year, 1–366 (BCD) |
| other | 0 (no year, control functions or straight binary seconds) |

B122 has no summer time flag. With automatic summer time the time in the signal jumps one hour forward or back at the change; in the Netherlands the hour from 02:00 to 03:00 is therefore sent twice in October. If you don't want that, switch summer time off (or choose UTC+00:00 for UTC).

### Time and synchronisation

- The ESP32 synchronises with an NTP server every 5 minutes (default `pool.ntp.org`).
- When a PCF8583 real-time clock is connected, it is the master clock (see below).
- A hardware timer drives the DAC at 40 kHz (40 samples per sine period). Every bit starts on a positive zero crossing of the sine.
- At every frame start the firmware compares the moment with the master clock (PCF8583 or NTP) and corrects if needed in steps of 25 µs, at most one step per bit. If the deviation exceeds 100 ms, the output restarts on the next second.
- If WiFi or NTP is lost, the output keeps running on the ESP32's internal clock (drift in the order of a few tens of ms per hour).
- The accuracy is that of NTP over WiFi: roughly 1–10 ms relative to UTC. WiFi power saving is switched off for this: with power saving on, NTP replies wait for the next beacon and the time can be off by a few hundred ms.

### PCF8583 real-time clock (optional)

With a battery-backed PCF8583 the clock starts right after power-up, even without WiFi.

- **At start-up** the firmware looks for the PCF8583 on I2C. If it holds a valid time, the IRIG-B output starts on that time straight away. If not, the clock waits for NTP, sets the PCF8583 and then starts.
- **From then on the PCF8583 is the master clock.** The frames run on the PCF8583's 1 Hz pulse (INT pin); the ESP32 clock is not used for that. The 1 Hz edges do not fall on the PCF8583's seconds tick, so after each edge the firmware reads the hundredths register until it changes and calculates where in the PCF second the edge falls (about 0.1 ms precision).
- **NTP corrects the PCF8583.** After every NTP sync the deviation is measured and logged. If two checks in a row exceed 20 ms (`RTC_SET_THRESHOLD_US`) in the same direction, the PCF8583 is set again, exactly on the second boundary. A single bad NTP sample therefore never moves the master clock. Two exceptions: the first NTP sync after switching NTP back on with the SET button, and a deviation of more than 0.5 s (`RTC_IMMEDIATE_US`), are corrected straight away.
- **No PCF8583 found**, or no 1 Hz pulse on INT: everything works as without a PCF8583, on NTP.
- The PCF8583 holds UTC. The full year and a validity marker are stored in its RAM (the PCF8583 itself only counts 2 year bits). A PCF8583 set by another device or program is therefore seen as invalid and set from NTP.

### OLED display (optional)

When a display is connected, it shows:

```
┌────────────────────────────┐
│      Mon 05 Oct 2026       │   date
│                            │
│        14:23:07            │   time (large)
│                            │
│ ▂▄▆█               IRIG on │   WiFi signal, IRIG output
└────────────────────────────┘
```

- The time is exactly the time in the IRIG-B frames (local time per the configured time zone), updated right after every second boundary.
- Bottom line: WiFi signal strength at the left (4 bars; empty bars with a cross = no WiFi), `NTP off` in the middle when NTP sync is switched off, and the IRIG output state at the right.
- As long as there is no valid time yet, it shows `--:--:--` with the reason (`Connecting to WiFi` or `Waiting for NTP`).
- While the configuration portal is active, the display shows the WiFi network name and the address `192.168.4.1`.
- Against burn-in, the content shifts one pixel every 10 minutes.
- No display connected: everything works as without a display.

## Hardware

- ESP32 DevKit (classic ESP32, for example ESP32-WROOM-32). Variants such as S2, S3 and C3 do not work: the firmware uses the DAC of the classic ESP32.
- Power over USB. Use a good cable and power supply: when WiFi starts, the ESP32 briefly draws 300–500 mA. A weak supply causes `Brownout detector was triggered` and a reboot loop. An electrolytic capacitor of 100–470 µF between 5V and GND helps.

### Connections

| Function | GPIO |
|---|---|
| IRIG-B output (DAC) | 25 |
| Status LED (on the board) | 2 |
| I2C SDA: PCF8583 and OLED display (optional) | 21 |
| I2C SCL: PCF8583 and OLED display (optional) | 22 |
| PCF8583 INT (optional) | 4 |
| Button SET (optional) | 5 |
| Button UP (optional) | 18 |
| Button DOWN (optional) | 19 |

### IRIG-B output

GPIO25 carries the AM signal around 1.65 V: about 3.1 Vpp during mark and 1.0 Vpp during space. The DAC can hardly source any current, so do not connect a receiver directly.

**High-impedance input (10 kΩ or more):** a low-pass filter and a coupling capacitor are enough.

```
 GPIO25 ──[ 1k ]──┬──||──────── IRIG-B out
                  │  10µF
                 ═╪═ 22nF       (+ of the capacitor towards the ESP32)
                  │
 GND ─────────────┴──────────── GND
```

**600 Ω input or long cable:** add an op-amp buffer, for example an MCP6002 (rail-to-rail, powered from 5V).

```
                        5V
                        │
                     ┌──┴──┐
 GPIO25 ──[ 1k ]──┬──┤+    │
                  │  │ op- ├──┬──||──[ 100Ω ]──── IRIG-B out
                 ═╪═ │ amp │  │  10µF
             22nF │ ┌┤-    │  │
                  │ │└──┬──┘  │
                  │ └───┼─────┘  (output back to -: voltage follower)
                  │     │
 GND ─────────────┴─────┴──────────────────────── GND
```

- 1 kΩ / 22 nF forms a low-pass filter (about 7 kHz) that removes the 40 kHz sample steps.
- The 10 µF coupling capacitor removes the 1.65 V DC level.
- The 100 Ω series resistor protects the op-amp against short circuits and capacitive loads.

The signal level (`IRIG_AMP_MARK`) and modulation ratio (`IRIG_MARK_SPACE`) are set in `src/config.h`.

### PCF8583

| PCF8583 | To |
|---|---|
| VDD (pin 8) | 3V3 |
| VSS (pin 4) | GND |
| SDA (pin 5) | GPIO21, 4.7 kΩ pull-up to 3V3 |
| SCL (pin 6) | GPIO22, 4.7 kΩ pull-up to 3V3 |
| INT (pin 7) | GPIO4, 10 kΩ pull-up to 3V3 (open drain, 1 Hz) |
| A0 (pin 3) | GND (I2C address 0x50) |
| OSCI / OSCO (pin 1 / 2) | 32.768 kHz crystal |

Ready-made modules often already have the pull-ups. Give the PCF8583 a backup battery (for example a CR2032 through a diode to VDD), otherwise the time is lost after a power interruption. Pins and address are in `src/config.h`.

### OLED display

GM009605 v4.3: 0.96" OLED, 128×64, SSD1306, I2C.

| Display | To |
|---|---|
| GND | GND |
| VCC | 3V3 |
| SCL | GPIO22 |
| SDA | GPIO21 |

The display shares the I2C bus with the PCF8583. The module has its own pull-ups, so together with a PCF8583 extra resistors are usually not needed. The default address is 0x3C; if the address jumper on the back is set to 0x7A, set `DISPLAY_I2C_ADDR` in `src/config.h` to 0x3D. Brightness (`DISPLAY_CONTRAST`) and 180° rotation (`DISPLAY_FLIP`) are set there too.

### Buttons

Three push buttons, each between its GPIO and GND (the internal pull-ups are used, no resistors needed):

| Button | GPIO | Other side |
|---|---|---|
| SET | 5 | GND |
| UP | 18 | GND |
| DOWN | 19 | GND |

Pins and timing are in `src/config.h`. Setting the time needs the OLED display to see what you are doing; switching NTP on/off also works without it.

## Operation

### First-time setup

1. Power up the ESP32. If no WiFi settings are stored yet, the configuration portal starts by itself (LED blinks fast).
2. Connect your phone or laptop to the WiFi network `IRIGB_<number>`. The configuration page opens by itself; if not, go to `http://192.168.4.1`.
3. Choose your WiFi network and enter the password. Optionally set a fixed IP address and a different NTP server, and choose the time zone and whether summer time is automatic.
4. Press **Save and connect**. The ESP32 connects, synchronises via NTP and starts the IRIG-B output.

### Changing settings later

Hold **SET** while powering up (or while pressing the reset button) to open the portal again; you can release it once the ESP32 has started. Pressing SET once more closes the portal without saving. The BOOT button on the board is not used.

To change only the time zone, summer time or NTP server, leave network name and password empty and press **Save and connect**: the ESP32 keeps the current WiFi connection.

### Time zone and summer time

| Setting | Options | Default |
|---|---|---|
| Time zone | UTC−12:00 to UTC+14:00, including half and quarter hours | UTC+01:00 |
| Automatic summer time | on / off | on |

Automatic summer time follows the European rules: from the last Sunday of March to the last Sunday of October, changing at 01:00 UTC. For time zones outside Europe with different summer time rules, switch it off. A new time zone takes effect immediately, for the IRIG-B signal and the display. The PCF8583 always holds UTC and does not need to be set again.

### Setting the time by hand and switching NTP off

| Button | Action |
|---|---|
| SET long (1 s) | Start setting date and time |
| SET short | While setting: next field; on the seconds field: save. Otherwise: NTP sync on/off |
| SET long while setting | Cancel without saving |
| UP / DOWN | While setting: change the underlined field; hold to repeat |

- The fields are, in order: day, month, year, hour, minute, second, in local time. They start at the current time.
- The time is set at the moment of the last press. To set it to the second, set the seconds a little ahead and press SET when a reference clock reaches that second.
- With a PCF8583 the time is written to the PCF8583 and kept after a power cut (with backup battery). Without a PCF8583 the ESP32 clock is set; that time is lost at a reboot.
- Saving a manual time also switches NTP sync off, otherwise NTP would overwrite it within minutes. The display shows `NTP off`.
- NTP off is remembered across reboots. A short press on SET switches it back on.
- After 60 s without a button press, setting the time stops without saving.
- With NTP off and without a PCF8583, the clock has no time after a reboot: the display shows `Hold SET to set time`.

### Status LED

| LED | Meaning |
|---|---|
| Fast blinking | Configuration portal active |
| Slow blinking | Connecting to WiFi |
| Steady on | WiFi connected, waiting for NTP (or a valid PCF8583 time) |
| Heartbeat (short off every second) | IRIG-B output running |

### Serial monitor

At 115200 baud the ESP32 prints status messages, among others `[NTP] synced` at every sync, `[RTC]` messages from the PCF8583 (including the deviation from NTP), `[I2C]` at start-up with the devices found on the bus (`0x50` = PCF8583, `0x3C` = display), `[OLED]` at start-up (display found or not) and every minute a `[STAT]` line with time, master clock, WiFi status and the phase deviation of the signal.

## Building

- **PlatformIO**: `pio run` builds the firmware and puts the result in `publish/`; `pio run -t upload` flashes directly. Requires the pioarduino platform (Arduino-ESP32 core 3.x). PlatformIO fetches U8g2 itself (`lib_deps` in `platformio.ini`).
- **Arduino IDE**: Arduino-ESP32 core 3.x, board *ESP32 Dev Module*. Install the **U8g2** library (olikraus) through the Library Manager. The IDE requires the folder to have the same name as the `.ino`, so open the project from a folder named `IRIG_B_Masterclock`.

### Project structure

| File | Content |
|---|---|
| `IRIG_B_Masterclock.ino` | start-up, button, WiFi, NTP, settings, status |
| `src/config.h` | pins, pulse widths, amplitude, default time zone, sync settings |
| `src/IrigB.cpp` | IRIG-B generator (timer, DAC, frame building, synchronisation) |
| `src/TimeRef.cpp` | master clock: PCF8583 (I2C and 1 Hz pulse) or NTP |
| `src/TimeZone.cpp` | time zone and summer time (POSIX TZ string, portal drop-down) |
| `src/Display.cpp` | OLED display (U8g2) |
| `src/I2cBus.cpp` | shared I2C bus for PCF8583 and display |
| `src/Menu.cpp` | front panel buttons: manual time setting, NTP on/off |
| `src/StatusLed.cpp` | LED patterns |
| `src/WiFiConnect.cpp` | captive portal |
| `tools/publish.py` | copies the firmware to `publish/` after the build |

The captive portal (`src/WiFiConnect.*`) comes from the TEF6686_ESP32 project and is licensed under GPL v3.
