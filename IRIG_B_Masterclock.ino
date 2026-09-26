/*
   IRIG-B122 master clock for ESP32

   Sends IRIG-B122 (100 pps, 1 kHz AM sine, BCD time-of-year) on the DAC
   output (GPIO25). Time comes from NTP over WiFi and is sent as Dutch
   local time with automatic summer/winter time (TIMEZONE in config.h).

   BOOT button: press after start-up to open the WiFi configuration portal.
   Onboard LED:
     fast blink   captive portal active
     slow blink   connecting to WiFi
     steady on    WiFi connected, waiting for NTP
     heartbeat    IRIG-B output running (short off at every second)

   Board: ESP32 DevKit (esp32dev), Arduino-ESP32 core 3.x
*/
#include <WiFi.h>
#include <Preferences.h>
#include <esp_sntp.h>

#include "src/config.h"
#include "src/IrigB.h"
#include "src/StatusLed.h"
#include "src/WiFiConnect.h"

WiFiConnect wc;
Preferences prefs;

char cfgNtp[64];
bool cfgStaticIP;
uint32_t cfgIP, cfgGateway, cfgSubnet;

volatile bool ntpSynced = false;
volatile uint32_t ntpSyncCount = 0;
volatile bool gotIP = false;
uint32_t ntpStartedAt = 0;

// ---- Settings ----

void loadSettings() {
  prefs.begin("irigb", false);   // read-write, so the namespace is created on first boot
  strlcpy(cfgNtp, prefs.getString("ntp", DEFAULT_NTP).c_str(), sizeof(cfgNtp));
  cfgStaticIP = prefs.getBool("static", false);
  cfgIP = prefs.getUInt("ip", 0);
  cfgGateway = prefs.getUInt("gw", 0);
  cfgSubnet = prefs.getUInt("sn", 0);
  prefs.end();
}

void saveSettings() {
  prefs.begin("irigb", false);
  prefs.putString("ntp", cfgNtp);
  prefs.putBool("static", cfgStaticIP);
  prefs.putUInt("ip", cfgIP);
  prefs.putUInt("gw", cfgGateway);
  prefs.putUInt("sn", cfgSubnet);
  prefs.end();
}

// ---- WiFi and NTP ----

void onNtpSync(struct timeval *tv) {
  ntpSynced = true;
  ntpSyncCount = ntpSyncCount + 1;
}

void startNtp() {
  esp_sntp_stop();
  sntp_set_sync_interval(NTP_INTERVAL_MS);
  sntp_set_time_sync_notification_cb(onNtpSync);
  configTzTime(TIMEZONE, cfgNtp);
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

  static WiFiConnectParam ntpText("NTP-server");
  static WiFiConnectParam ntpInput("ntp", DEFAULT_NTP, cfgNtp, sizeof(cfgNtp) - 1);
  static bool paramsAdded = false;
  if (!paramsAdded) {
    wc.addParameter(&ntpText);
    wc.addParameter(&ntpInput);
    paramsAdded = true;
  }

  wc.setStaticIP(cfgStaticIP, cfgIP, cfgGateway, cfgSubnet);
  bool connected = wc.startConfigurationPortal(PIN_BUTTON);

  if (connected) {
    if (strlen(ntpInput.getValue()) > 0) strlcpy(cfgNtp, ntpInput.getValue(), sizeof(cfgNtp));
    cfgStaticIP = wc.getStaticIPEnabled();
    cfgIP = wc.getStaticIP();
    cfgGateway = wc.getGateway();
    cfgSubnet = wc.getSubnetMask();
    saveSettings();
    Serial.println("[WiFi] portal: settings saved");
  } else {
    Serial.println("[WiFi] portal cancelled");
  }

  // Wait for the button to be released so it does not re-open the portal
  while (digitalRead(PIN_BUTTON) == LOW) delay(10);

  if (connected) {
    gotIP = true;      // restart NTP, the server may have changed
  } else {
    startWiFi();
  }
}

// ---- Button ----

bool buttonPressed() {
  static bool lastState = HIGH;
  static uint32_t changedAt = 0;
  static bool reported = false;

  bool state = digitalRead(PIN_BUTTON);
  if (state != lastState) {
    lastState = state;
    changedAt = millis();
    reported = false;
  }
  if (state == LOW && !reported && millis() - changedAt > 50) {
    reported = true;
    return true;
  }
  return false;
}

// ---- Main ----

void setup() {
  Serial.begin(115200);
  Serial.println("\nIRIG-B122 master clock");

  pinMode(PIN_BUTTON, INPUT_PULLUP);
  statusLedBegin();
  statusLedSet(LED_CONNECTING);

  loadSettings();
  irigBegin();

  WiFi.onEvent(onWiFiGotIP, ARDUINO_EVENT_WIFI_STA_GOT_IP);
  startWiFi();

  // Nothing configured yet: open the portal straight away
  if (!hasWiFiCredentials()) runPortal();
}

void loop() {
  if (buttonPressed()) runPortal();

  bool wifiUp = WiFi.status() == WL_CONNECTED;

  if (gotIP) {
    gotIP = false;
    Serial.printf("[WiFi] connected, IP %s\n", WiFi.localIP().toString().c_str());
    if (wifiUp) startNtp();
  }

  // Not synced yet while connected: start NTP if the IP event was missed,
  // or restart it every 30 s (bypasses the long lwIP retry back-off)
  if (!ntpSynced && wifiUp && (ntpStartedAt == 0 || millis() - ntpStartedAt > 30000)) {
    Serial.println(ntpStartedAt == 0 ? "[NTP] starting" : "[NTP] no response, retrying");
    startNtp();
  }

  // Output starts on the first NTP sync and then keeps running on the
  // internal clock, also when WiFi or NTP is lost.
  if (ntpSynced && !irigRunning()) irigEnable(true);

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
  }

  static uint32_t lastLog = millis();
  if (millis() - lastLog >= (irigRunning() ? 60000UL : 10000UL)) {
    lastLog = millis();
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    Serial.printf("[STAT] %04d-%02d-%02d %02d:%02d:%02d day %03d, WiFi %s, NTP %s (%lu syncs), IRIG %s, phase %ld us\n",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                  tm.tm_yday + 1, WiFi.status() == WL_CONNECTED ? "ok" : "down",
                  ntpSynced ? "ok" : "waiting", (unsigned long)ntpSyncCount,
                  irigRunning() ? "on" : "off", (long)irigLastErrorUs());
  }

  delay(10);
}
