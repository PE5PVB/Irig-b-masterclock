/*
   IRIG-B122 master clock for ESP32

   Sends IRIG-B122 (100 pps, 1 kHz AM sine, BCD time-of-year) on the DAC
   output (GPIO25). Time comes from NTP over WiFi and is sent as local time:
   UTC offset and automatic (European) summer time are set in the portal.

   Optional PCF8583 real-time clock (I2C + 1 Hz INT): when it holds a valid
   time the output starts straight away and runs on the PCF8583; NTP then
   only corrects it. Without a PCF8583 the output runs on NTP.

   Front panel buttons (SET, UP, DOWN): SET held during boot = WiFi
   configuration portal, SET long = set date/time manually, SET short = NTP
   sync on/off (remembered). See src/Menu.h.
   Onboard LED:
     fast blink   captive portal active
     slow blink   connecting to WiFi
     steady on    WiFi connected, waiting for NTP (or a valid PCF8583 time)
     heartbeat    IRIG-B output running (short off at every second)

   Optional OLED display (GM009605 / SSD1306 128x64, I2C): shows the time,
   date and status.

   Board: ESP32 DevKit (esp32dev), Arduino-ESP32 core 3.x
*/
#include <WiFi.h>
#include <Preferences.h>
#include <esp_sntp.h>

#include "src/config.h"
#include "src/IrigB.h"
#include "src/StatusLed.h"
#include "src/TimeRef.h"
#include "src/Display.h"
#include "src/TimeZone.h"
#include "src/Menu.h"
#include "src/WiFiConnect.h"

WiFiConnect wc;
Preferences prefs;

char cfgNtp[64];
int cfgTzOffset;       // minutes east of UTC
bool cfgDst;           // automatic European summer time
char tzString[48];     // POSIX TZ string built from the two above
bool cfgStaticIP;
uint32_t cfgIP, cfgGateway, cfgSubnet;
bool cfgNtpOff;        // NTP sync switched off with the SET button

volatile bool ntpSynced = false;
volatile uint32_t ntpSyncCount = 0;
volatile bool gotIP = false;
uint32_t ntpStartedAt = 0;
bool manualTimeSet = false;   // system clock set by hand (used when there is no PCF8583)
bool ntpJustEnabled = false;  // first NTP sync after switching NTP on: trust it

// ---- Settings ----

void loadSettings() {
  prefs.begin("irigb", false);   // read-write, so the namespace is created on first boot
  strlcpy(cfgNtp, prefs.getString("ntp", DEFAULT_NTP).c_str(), sizeof(cfgNtp));
  cfgTzOffset = prefs.getInt("tzoff", DEFAULT_TZ_OFFSET_MIN);
  cfgDst = prefs.getBool("dst", DEFAULT_DST);
  if (!timeZoneValidOffset(cfgTzOffset)) cfgTzOffset = DEFAULT_TZ_OFFSET_MIN;
  cfgStaticIP = prefs.getBool("static", false);
  cfgIP = prefs.getUInt("ip", 0);
  cfgGateway = prefs.getUInt("gw", 0);
  cfgSubnet = prefs.getUInt("sn", 0);
  cfgNtpOff = prefs.getBool("ntpoff", false);
  prefs.end();
}

void saveSettings() {
  prefs.begin("irigb", false);
  prefs.putString("ntp", cfgNtp);
  prefs.putInt("tzoff", cfgTzOffset);
  prefs.putBool("dst", cfgDst);
  prefs.putBool("static", cfgStaticIP);
  prefs.putUInt("ip", cfgIP);
  prefs.putUInt("gw", cfgGateway);
  prefs.putUInt("sn", cfgSubnet);
  prefs.putBool("ntpoff", cfgNtpOff);
  prefs.end();
}

// ---- Time zone ----

void applyTimeZone() {
  timeZoneString(cfgTzOffset, cfgDst, tzString, sizeof(tzString));
  setenv("TZ", tzString, 1);
  tzset();
  Serial.printf("[TZ] %s\n", tzString);
}

// ---- WiFi and NTP ----

void onNtpSync(struct timeval *tv) {
  ntpSynced = true;
  ntpSyncCount = ntpSyncCount + 1;
}

void startNtp() {
  if (cfgNtpOff) return;
  esp_sntp_stop();
  sntp_set_sync_interval(NTP_INTERVAL_MS);
  sntp_set_time_sync_notification_cb(onNtpSync);
  configTzTime(tzString, cfgNtp);
  ntpStartedAt = millis();
  Serial.printf("[NTP] server %s\n", cfgNtp);
}

// SNTP is (re)started once WiFi has an IP address; started earlier, the
// first request fails and lwIP backs off for a long time.
void onWiFiGotIP(arduino_event_id_t event) {
  gotIP = true;
}

void startWiFi() {
  WiFi.mode(WIFI_STA);
  // No modem sleep: with power save on, NTP replies wait for the next beacon
  // and the NTP time can be off by up to a few hundred ms
  WiFi.setSleep(false);
  setWiFiCountryWorldwide();
  if (cfgStaticIP) {
    WiFi.config(IPAddress(cfgIP), IPAddress(cfgGateway), IPAddress(cfgSubnet), IPAddress(cfgGateway));
  } else {
    WiFi.config(IPAddress((uint32_t)0), IPAddress((uint32_t)0), IPAddress((uint32_t)0));
  }
  WiFi.setAutoReconnect(true);
  WiFi.begin();
}

bool hasWiFiCredentials() {
  wifi_config_t conf;
  esp_wifi_get_config(WIFI_IF_STA, &conf);
  return conf.sta.ssid[0] != 0;
}

void runPortal() {
  Serial.println("[WiFi] captive portal started");
  statusLedSet(LED_PORTAL);
  displaySetPortal(true);

  static WiFiConnectParam ntpText("NTP-server");
  static WiFiConnectParam ntpInput("ntp", DEFAULT_NTP, cfgNtp, sizeof(cfgNtp) - 1);
  static WiFiConnectParam tzSelect("tz", 8);
  static WiFiConnectParam dstCheck("dst", 2);
  static WiFiConnectParam keepText("Only changing the time settings? Leave network name and password "
                                   "empty to keep the current WiFi connection.");
  static bool paramsAdded = false;
  if (!paramsAdded) {
    wc.addParameter(&ntpText);
    wc.addParameter(&ntpInput);
    wc.addParameter(&tzSelect);
    wc.addParameter(&dstCheck);
    if (hasWiFiCredentials()) wc.addParameter(&keepText);
    paramsAdded = true;
  }

  // Time zone fields, filled with the current setting
  static String tzHtml, dstHtml;
  tzHtml = "<p class='pt'>Time zone</p><select id='tz' name='tz'>";
  tzHtml += timeZoneOptionsHtml(cfgTzOffset);
  tzHtml += "</select>";
  dstHtml = "<label class='hn'><input type='checkbox' id='dst' name='dst' value='1'";
  if (cfgDst) dstHtml += " checked";
  dstHtml += "> Automatic summer time (European rules)</label>";
  tzSelect.setCustomHTML(tzHtml.c_str());
  dstCheck.setCustomHTML(dstHtml.c_str());

  wc.setStaticIP(cfgStaticIP, cfgIP, cfgGateway, cfgSubnet);
  bool connected = wc.startConfigurationPortal(PIN_BTN_SET);   // SET closes the portal

  if (connected) {
    if (strlen(ntpInput.getValue()) > 0) strlcpy(cfgNtp, ntpInput.getValue(), sizeof(cfgNtp));
    if (strlen(tzSelect.getValue()) > 0) {
      int offset = atoi(tzSelect.getValue());
      if (timeZoneValidOffset(offset)) cfgTzOffset = offset;
    }
    cfgDst = strcmp(dstCheck.getValue(), "1") == 0;
    cfgStaticIP = wc.getStaticIPEnabled();
    cfgIP = wc.getStaticIP();
    cfgGateway = wc.getGateway();
    cfgSubnet = wc.getSubnetMask();
    saveSettings();
    applyTimeZone();
    Serial.println("[WiFi] portal: settings saved");
  } else {
    Serial.println("[WiFi] portal cancelled");
  }

  displaySetPortal(false);

  // Wait for SET to be released, so the menu does not see it as a press
  while (digitalRead(PIN_BTN_SET) == LOW) delay(10);

  if (connected) {
    gotIP = true;      // restart NTP, the server may have changed
  } else {
    startWiFi();
  }
}

// ---- Front panel ----

void setNtpOff(bool off) {
  cfgNtpOff = off;
  saveSettings();
  if (off) {
    esp_sntp_stop();
    ntpStartedAt = 0;
    Serial.println("[NTP] sync switched off");
  } else {
    Serial.println("[NTP] sync switched on");
    ntpJustEnabled = true;
    if (WiFi.status() == WL_CONNECTED) startNtp();
  }
}

void handleMenu(MenuEvent event) {
  if (event == MENU_TOGGLE_NTP) {
    setNtpOff(!cfgNtpOff);
    menuToast(cfgNtpOff ? "NTP sync off" : "NTP sync on");
  } else if (event == MENU_SET_TIME) {
    int64_t epoch = menuSetEpoch();
    int64_t pressUs = menuSetPressUs();
    if (timeRefUsingRtc()) {
      timeRefSetRtc(epoch, pressUs);             // the PCF8583 is the master clock
    } else {
      int64_t us = epoch * 1000000 + (esp_timer_get_time() - pressUs);
      struct timeval tv = { (time_t)(us / 1000000), (suseconds_t)(us % 1000000) };
      settimeofday(&tv, NULL);
      manualTimeSet = true;
    }
    // NTP would overwrite the manual time within minutes
    if (!cfgNtpOff) setNtpOff(true);
    menuToast(timeRefUsingRtc() ? "Time set, NTP off" : "Time set (not kept)");
  }
}

// ---- Main ----

void setup() {
  // SET held during boot: WiFi configuration portal. Sampled first thing,
  // so it may be released while the rest starts up.
  pinMode(PIN_BTN_SET, INPUT_PULLUP);
  delay(20);
  bool setHeldAtBoot = digitalRead(PIN_BTN_SET) == LOW;

  Serial.begin(115200);
  Serial.println("\nIRIG-B122 master clock");

  statusLedBegin();
  statusLedSet(LED_CONNECTING);

  loadSettings();
  applyTimeZone();
  timeRefBegin();
  displayBegin();
  irigBegin();

  menuBegin();
  if (cfgNtpOff) Serial.println("[NTP] sync is switched off");

  WiFi.onEvent(onWiFiGotIP, ARDUINO_EVENT_WIFI_STA_GOT_IP);
  startWiFi();

  // SET held during boot, or nothing configured yet: open the portal
  if (setHeldAtBoot || !hasWiFiCredentials()) runPortal();
}

void loop() {
  bool wifiUp = WiFi.status() == WL_CONNECTED;

  if (gotIP) {
    gotIP = false;
    Serial.printf("[WiFi] connected, IP %s\n", WiFi.localIP().toString().c_str());
    if (wifiUp) startNtp();
  }

  // Not synced yet while connected: start NTP if the IP event was missed,
  // or restart it every 30 s (bypasses the long lwIP retry back-off)
  if (!cfgNtpOff && !ntpSynced && wifiUp && (ntpStartedAt == 0 || millis() - ntpStartedAt > 30000)) {
    Serial.println(ntpStartedAt == 0 ? "[NTP] starting" : "[NTP] no response, retrying");
    startNtp();
  }

  // Output starts on a valid PCF8583 time, the first NTP sync or a manually
  // set time, and then keeps running, also when WiFi or NTP is lost.
  bool timeValid = timeRefRtcValid() || ntpSynced || manualTimeSet;
  if (timeValid && !irigRunning()) irigEnable(true);
  displaySetStatus(wifiUp, wifiUp ? WiFi.RSSI() : 0, timeValid, irigRunning(), cfgNtpOff);

  handleMenu(menuLoop(timeRefNowUs(), timeValid));

  if (irigRunning()) {
    statusLedSet(LED_HEARTBEAT);
  } else if (WiFi.status() == WL_CONNECTED) {
    statusLedSet(LED_CONNECTED);
  } else {
    statusLedSet(LED_CONNECTING);
  }

  static uint32_t lastSyncCount = 0;
  if (ntpSyncCount != lastSyncCount) {
    lastSyncCount = ntpSyncCount;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    Serial.printf("[NTP] synced, local time %02d:%02d:%02d\n", tm.tm_hour, tm.tm_min, tm.tm_sec);
    timeRefSyncRtc(ntpJustEnabled);   // correct the PCF8583, if present
    ntpJustEnabled = false;
  }

  static uint32_t lastLog = millis();
  if (millis() - lastLog >= (irigRunning() ? 60000UL : 10000UL)) {
    lastLog = millis();
    time_t now = (time_t)(timeRefNowUs() / 1000000);
    struct tm tm;
    localtime_r(&now, &tm);
    Serial.printf("[STAT] %04d-%02d-%02d %02d:%02d:%02d day %03d, clock %s, WiFi %s, NTP %s (%lu syncs), IRIG %s, phase %ld us\n",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                  tm.tm_yday + 1, timeRefRtcValid() ? "PCF8583" : (cfgNtpOff ? "manual" : "NTP"),
                  WiFi.status() == WL_CONNECTED ? "ok" : "down",
                  cfgNtpOff ? "off" : (ntpSynced ? "ok" : "waiting"), (unsigned long)ntpSyncCount,
                  irigRunning() ? "on" : "off", (long)irigLastErrorUs());
  }

  delay(10);
}
