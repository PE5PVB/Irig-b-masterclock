# Irig-b masterclock

IRIG-B masterclock op basis van een ESP32. De klok haalt de tijd via NTP over WiFi en stuurt die continu uit als **IRIG-B122**: een 1 kHz sinus, amplitude-gemoduleerd met de tijdcode. Apparatuur met een IRIG-B ingang (tijdcode-lezers, recorders, meetsystemen, klokken) kan hierop synchroniseren.

De uitgestuurde tijd is **Nederlandse tijd** met automatische zomer- en wintertijd.

## Hoe het werkt

### IRIG-B122 in het kort

Elke seconde wordt één frame van 100 bits verstuurd, elke bit duurt 10 ms. Een bit is een stuk 1 kHz sinus (10 perioden) waarvan het begin met hoge amplitude (mark) wordt verstuurd en de rest met lage amplitude (space). De lengte van het hoge deel bepaalt de betekenis:

| Symbool | Hoog deel | Gebruik |
|---|---|---|
| 0 | 2 ms | databit 0 |
| 1 | 5 ms | databit 1 |
| Marker | 8 ms | frame-begin (Pr, bit 0) en scheiding na elk veld (bit 9, 19, …, 99) |

```
 bit:   |<------------- 10 ms ------------->|
 "0":   |MMMM|ssssssssssssssssssssssssssssss|     M = mark  (hoge amplitude)
 "1":   |MMMMMMMMMM|ssssssssssssssssssssssss|     s = space (lage amplitude)
 marker:|MMMMMMMMMMMMMMMM|ssssssssssssssssss|
```

Mark en space verhouden zich als 3:1. Het frame begint met twee markers achter elkaar (P0 van het vorige frame en Pr). De voorflank van Pr valt precies op de secondegrens: dat is het tijdstip dat het frame aangeeft.

Frame-inhoud (B122 = alleen BCD tijd-van-het-jaar):

| Bits | Inhoud |
|---|---|
| 1–8 | seconden (BCD) |
| 10–17 | minuten (BCD) |
| 20–26 | uren (BCD) |
| 30–41 | dag van het jaar, 1–366 (BCD) |
| overige | 0 (geen jaar, control functions of straight binary seconds) |

B122 heeft geen zomertijdvlag. Bij de overgang springt de tijd in het signaal een uur vooruit of terug; in oktober wordt het uur van 02:00 tot 03:00 dus twee keer verstuurd.

### Tijd en synchronisatie

- De ESP32 synchroniseert elke 5 minuten met een NTP-server (standaard `pool.ntp.org`).
- Is er een PCF8583 real-time clock aangesloten, dan is die de hoofdklok (zie hieronder).
- Een hardware timer stuurt de DAC op 40 kHz aan (40 samples per sinusperiode). Elke bit begint op een positieve nuldoorgang van de sinus.
- Bij elke framestart vergelijkt de firmware het tijdstip met de hoofdklok (PCF8583 of NTP) en corrigeert zo nodig in stappen van 25 µs, maximaal één stap per bit. Is de afwijking groter dan 100 ms, dan start de uitvoer opnieuw op de volgende seconde.
- Valt WiFi of NTP weg, dan loopt de uitvoer door op de interne klok van de ESP32 (drift in de orde van enkele tientallen ms per uur).
- De nauwkeurigheid is die van NTP over WiFi: ruwweg 1–10 ms ten opzichte van UTC.

### PCF8583 real-time clock (optioneel)

Met een PCF8583 met backup-batterij start de klok direct na het aanzetten, ook zonder WiFi.

- **Bij het opstarten** zoekt de firmware de PCF8583 op I2C. Staat er een geldige tijd in, dan start de IRIG-B uitvoer meteen op die tijd. Zo niet, dan wacht de klok op NTP, zet de PCF8583 gelijk en start dan.
- **Daarna is de PCF8583 de hoofdklok.** De frames lopen op de 1 Hz puls van de PCF8583 (INT-pin); de klok van de ESP32 wordt daarvoor niet gebruikt.
- **NTP stelt de PCF8583 bij.** Na elke NTP-synchronisatie wordt de afwijking gemeten en gelogd. Is die groter dan 20 ms (`RTC_SET_THRESHOLD_US`), dan wordt de PCF8583 opnieuw gelijkgezet, precies op de secondegrens.
- **Geen PCF8583 gevonden**, of geen 1 Hz puls op INT: dan werkt alles zoals zonder PCF8583, op NTP.
- De PCF8583 bevat UTC. Het volledige jaartal en een geldigheidsmarkering staan in zijn RAM (de PCF8583 telt zelf maar 2 jaarbits). Een PCF8583 die door een ander apparaat of programma is ingesteld, ziet de firmware daarom als ongeldig en zet hem via NTP gelijk.

## Hardware

- ESP32 DevKit (klassieke ESP32, bijvoorbeeld ESP32-WROOM-32). Varianten zoals S2, S3 en C3 werken niet: de firmware gebruikt de DAC van de klassieke ESP32.
- Voeding via USB. Gebruik een goede kabel en voeding: bij het opstarten van WiFi trekt de ESP32 kort 300–500 mA. Een te slappe voeding geeft `Brownout detector was triggered` en een herstart-lus. Een elco van 100–470 µF tussen 5V en GND helpt.

### Aansluitingen

| Functie | GPIO |
|---|---|
| IRIG-B uitgang (DAC) | 25 |
| Status-LED (op het board) | 2 |
| BOOT-knop (op het board) | 0 |
| PCF8583 SDA (optioneel) | 21 |
| PCF8583 SCL (optioneel) | 22 |
| PCF8583 INT (optioneel) | 4 |

**PCF8583 aansluiten:**

| PCF8583 | Naar |
|---|---|
| VDD (pin 8) | 3V3 |
| VSS (pin 4) | GND |
| SDA (pin 5) | GPIO21, 4,7 kΩ pull-up naar 3V3 |
| SCL (pin 6) | GPIO22, 4,7 kΩ pull-up naar 3V3 |
| INT (pin 7) | GPIO4, 10 kΩ pull-up naar 3V3 (open drain, 1 Hz) |
| A0 (pin 3) | GND (I2C-adres 0x50) |
| OSCI / OSCO (pin 1 / 2) | 32,768 kHz kristal |

Voor een kant-en-klare module zijn de pull-ups vaak al aanwezig. Geef de PCF8583 een backup-batterij (bijvoorbeeld een CR2032 via een diode op VDD), anders is de tijd na een spanningsonderbreking weg. Pinnen en adres staan in `src/config.h`.

Op GPIO25 staat het AM-signaal rond 1,65 V: ongeveer 3,1 Vtt tijdens mark en 1,0 Vtt tijdens space. De DAC kan nauwelijks stroom leveren, dus sluit een ontvanger niet direct aan.

**Hoogohmige ingang (10 kΩ of meer):** een laagdoorlaatfilter en een koppelcondensator zijn genoeg.

```
 GPIO25 ──[ 1k ]──┬──||──────── IRIG-B uit
                  │  10µF
                 ═╪═ 22nF       (+ van de elco naar de ESP32-kant)
                  │
 GND ─────────────┴──────────── GND
```

**600 Ω ingang of lange kabel:** zet er een op-amp buffer achter, bijvoorbeeld een MCP6002 (rail-to-rail, gevoed uit 5V).

```
                        5V
                        │
                     ┌──┴──┐
 GPIO25 ──[ 1k ]──┬──┤+    │
                  │  │ op- ├──┬──||──[ 100Ω ]──── IRIG-B uit
                 ═╪═ │ amp │  │  10µF
             22nF │ ┌┤-    │  │
                  │ │└──┬──┘  │
                  │ └───┼─────┘  (uitgang terug naar -: spanningsvolger)
                  │     │
 GND ─────────────┴─────┴──────────────────────── GND
```

- 1 kΩ / 22 nF vormt een laagdoorlaatfilter (ca. 7 kHz) dat de 40 kHz samplestappen wegfiltert.
- De 10 µF koppelcondensator haalt de 1,65 V gelijkspanning weg.
- De 100 Ω serieweerstand beschermt de op-amp bij kortsluiting of capacitieve belasting.

Het signaalniveau (IRIG_AMP_MARK) en de modulatieverhouding (IRIG_MARK_SPACE) zijn in te stellen in `src/config.h`.

## Bediening

### Eerste keer instellen

1. Zet de ESP32 aan. Zijn er nog geen WiFi-gegevens, dan start de configuratie-portal vanzelf (LED knippert snel).
2. Verbind met je telefoon of laptop met het WiFi-netwerk `IRIGB_<nummer>`. De configuratiepagina opent vanzelf; zo niet, ga naar `http://192.168.4.1`.
3. Kies je WiFi-netwerk, vul het wachtwoord in en eventueel een vast IP-adres en een andere NTP-server.
4. Druk op **Opslaan en verbinden**. De ESP32 verbindt, synchroniseert via NTP en start de IRIG-B uitvoer.

### Later wijzigen

Druk na het opstarten kort op de **BOOT**-knop om de portal opnieuw te openen. Nog een keer drukken sluit de portal zonder op te slaan. Houd BOOT niet ingedrukt tijdens het aanzetten: dan start de ESP32 in de programmeermodus.

### Status-LED

| LED | Betekenis |
|---|---|
| Snel knipperen | Configuratie-portal actief |
| Langzaam knipperen | Verbinden met WiFi |
| Continu aan | WiFi verbonden, wacht op NTP (of op een geldige PCF8583-tijd) |
| Hartslag (kort uit op elke seconde) | IRIG-B uitvoer loopt |

### Seriële monitor

Op 115200 baud toont de ESP32 status-meldingen, onder andere `[NTP] synced` bij elke synchronisatie, `[RTC]` meldingen van de PCF8583 (inclusief de afwijking ten opzichte van NTP) en elke minuut een `[STAT]` regel met tijd, hoofdklok, WiFi-status en de fase-afwijking van het signaal.

## Zelf bouwen

- **PlatformIO**: `pio run` bouwt de firmware en zet het resultaat in `publish/`; `pio run -t upload` flasht direct. Vereist het pioarduino platform (Arduino-ESP32 core 3.x).
- **Arduino IDE**: Arduino-ESP32 core 3.x, board *ESP32 Dev Module*. De IDE wil dat de map dezelfde naam heeft als de `.ino`, dus open het project vanuit een map `IRIG_B_Masterclock`.

### Projectstructuur

| Bestand | Inhoud |
|---|---|
| `IRIG_B_Masterclock.ino` | opstarten, knop, WiFi, NTP, instellingen, status |
| `src/config.h` | pinnen, pulsduren, amplitude, tijdzone, sync-instellingen |
| `src/IrigB.cpp` | IRIG-B generator (timer, DAC, frame-opbouw, synchronisatie) |
| `src/TimeRef.cpp` | hoofdklok: PCF8583 (I2C en 1 Hz puls) of NTP |
| `src/StatusLed.cpp` | LED-patronen |
| `src/WiFiConnect.cpp` | captive portal |
| `tools/publish.py` | kopieert de firmware na de build naar `publish/` |

De captive portal (`src/WiFiConnect.*`) komt uit het TEF6686_ESP32 project en valt onder GPL v3.
