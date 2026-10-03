/*
 * RobotDyn ESP32R4 - 4-Zone Sprinkler Controller
 * Copyright (C) 2026 Tim Gray
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Features:
 * - Web configuration
 * - Persistent settings stored in ESP32 NVS
 * - Web-selectable timezone
 * - One watering start time and one set of watering days
 * - Four independently enabled zones with individual run times
 * - Zones run sequentially with a 10 second pressure stabilization gap
 * - SNTP clock
 * - 24-hour watering inhibit
 * - Manual zone buttons with release cooldown
 * - Saved WiFi configuration with automatic access-point fallback
 * - Web-based OTA firmware updates
 *
 * Board: RobotDyn ESP32R4 4-Relay
 * Target: ESP32 Dev Module
 */

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <Update.h>
#include <time.h>

// -------------------------------------------------
// NETWORK DEFAULTS
// -------------------------------------------------
// These are used only until WiFi settings are changed from the web page.
const char* DEFAULT_WIFI_SSID     = "";
const char* DEFAULT_WIFI_PASSWORD = "";

const char* AP_SSID = "SprinklerController";
const char* DEFAULT_AP_PASSWORD = "sprinkler123";

const char* FIRMWARE_VERSION = "1.1.1";

const unsigned long WIFI_CONNECT_TIMEOUT_MS = 20000;
const unsigned long WIFI_RETRY_INTERVAL_MS = 60000;

// -------------------------------------------------
// HARDWARE
// -------------------------------------------------
const int RELAY_PINS[4]  = {25, 26, 33, 32};
const int BUTTON_PINS[4] = {34, 35, 36, 39};
const int STATUS_LED     = 2;

#define NUM_ZONES 4

const unsigned long RELEASE_COOLDOWN_MS = 300;
const unsigned long ZONE_GAP_MS = 10000;

// -------------------------------------------------
// ZONE SETTINGS
// -------------------------------------------------
struct Zone {
  bool enabled = true;
  int runMinutes = 10;

  bool active = false;
  bool manualRun = false;
  unsigned long activateAt = 0;
};

Zone zones[NUM_ZONES];

// -------------------------------------------------
// GLOBAL SCHEDULE SETTINGS
// -------------------------------------------------
bool wateringDays[7] = {false, false, false, false, false, false, false};
int startHour = 6;
int startMinute = 0;

// POSIX timezone string used by configTzTime().
// Default is US Eastern with automatic daylight saving time.
String timezoneName = "Eastern";
String timezoneRule = "EST5EDT,M3.2.0/2,M11.1.0/2";

// Prevent the same schedule from starting more than once per calendar day.
int lastScheduleYear = -1;
int lastScheduleDay = -1;
int lastRunDate = 0;

// -------------------------------------------------
// WATERING SEQUENCE STATE
// -------------------------------------------------
enum SequenceState {
  SEQUENCE_IDLE,
  SEQUENCE_RUNNING_ZONE,
  SEQUENCE_GAP
};

SequenceState sequenceState = SEQUENCE_IDLE;
int sequenceZone = -1;
unsigned long sequenceStateStartedAt = 0;

// -------------------------------------------------
// GLOBALS
// -------------------------------------------------
WebServer server(80);
Preferences preferences;

bool inhibitActive = false;
time_t inhibitUntil = 0;

bool btnInCooldown[NUM_ZONES] = {false};
unsigned long btnReleaseTime[NUM_ZONES] = {0};

String wifiSSID = DEFAULT_WIFI_SSID;
String wifiPassword = DEFAULT_WIFI_PASSWORD;
String apPassword = DEFAULT_AP_PASSWORD;

bool apFallbackActive = false;
bool otaInProgress = false;
bool otaUploadStarted = false;
bool otaUpdateSuccess = false;
unsigned long apRecoveryConnectedAt = 0;
unsigned long lastWiFiRetryAt = 0;
unsigned long wifiDisconnectedAt = 0;

// -------------------------------------------------
// FORWARD DECLARATIONS
// -------------------------------------------------
void loadSettings();
void saveSettings();
void applyTimezone();
void connectWiFi();
void startFallbackAP();
void handleNetwork();
void saveNetworkSettings();
void stopAllZones();
void cancelWateringSequence();
void startWateringSequence();
void startSequenceZone(int zoneIndex);
void finishSequenceZone();
void advanceWateringSequence();
int findNextEnabledZone(int afterZone);

void handleRoot();
void handleConfig();
void handleSave();
void handleTime();
void handleStatus();
void handleInhibit();
void handleZoneToggle();
void handleStopSprinkler();
void handleNetworkPage();
void handleSaveNetwork();
void handleUpdatePage();
void handleFirmwareUpdate();
void handleFirmwareUpload();
void handle404();

// -------------------------------------------------
// SETUP
// -------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("[ESP32R4] Starting sprinkler controller...");

  for (int i = 0; i < NUM_ZONES; i++) {
    pinMode(RELAY_PINS[i], OUTPUT);
    digitalWrite(RELAY_PINS[i], LOW);
  }

  for (int i = 0; i < NUM_ZONES; i++) {
    pinMode(BUTTON_PINS[i], INPUT);
  }

  pinMode(STATUS_LED, OUTPUT);
  digitalWrite(STATUS_LED, LOW);

  loadSettings();

  connectWiFi();
  applyTimezone();

  server.on("/",        HTTP_GET,  handleRoot);
  server.on("/config",  HTTP_GET,  handleConfig);
  server.on("/save",    HTTP_POST, handleSave);
  server.on("/time",    HTTP_GET,  handleTime);
  server.on("/status",  HTTP_GET,  handleStatus);
  server.on("/inhibit", HTTP_GET,  handleInhibit);
  server.on("/zone",    HTTP_POST, handleZoneToggle);
  server.on("/stop",    HTTP_POST, handleStopSprinkler);
  server.on("/network", HTTP_GET,  handleNetworkPage);
  server.on("/network/save", HTTP_POST, handleSaveNetwork);
  server.on("/update", HTTP_GET, handleUpdatePage);
  server.on("/update", HTTP_POST, handleFirmwareUpdate, handleFirmwareUpload);
  server.onNotFound(handle404);

  server.begin();
  Serial.println("Web server ready.");
}

// -------------------------------------------------
// LOOP
// -------------------------------------------------
void loop() {
  server.handleClient();
  handleNetwork();
  handleStatusLED();
  handleButtons();
  handleSchedules();
  handleInhibitExpiry();
}

// -------------------------------------------------
// PERSISTENT SETTINGS
// -------------------------------------------------
void loadSettings() {
  preferences.begin("sprinkler", true);

  startHour = preferences.getInt("startHour", 6);
  startMinute = preferences.getInt("startMin", 0);

  timezoneName = preferences.getString("tzName", "Eastern");
  timezoneRule = preferences.getString("tzRule", "EST5EDT,M3.2.0/2,M11.1.0/2");

  lastRunDate = preferences.getInt("lastRun", 0);

  uint8_t dayMask = preferences.getUChar("days", 0);
  for (int d = 0; d < 7; d++) {
    wateringDays[d] = (dayMask & (1 << d)) != 0;
  }

  for (int i = 0; i < NUM_ZONES; i++) {
    String enabledKey = "z" + String(i) + "en";
    String runtimeKey = "z" + String(i) + "run";

    zones[i].enabled = preferences.getBool(enabledKey.c_str(), true);
    zones[i].runMinutes = preferences.getInt(runtimeKey.c_str(), 10);
  }

  uint64_t savedInhibitUntil = preferences.getULong64("inhibit", 0);
  inhibitUntil = (time_t)savedInhibitUntil;

  wifiSSID = preferences.getString("wifiSSID", DEFAULT_WIFI_SSID);
  wifiPassword = preferences.getString("wifiPass", DEFAULT_WIFI_PASSWORD);
  apPassword = preferences.getString("apPass", DEFAULT_AP_PASSWORD);

  preferences.end();

  Serial.println("Settings loaded from NVS.");
}

void saveSettings() {
  preferences.begin("sprinkler", false);

  preferences.putInt("startHour", startHour);
  preferences.putInt("startMin", startMinute);
  preferences.putString("tzName", timezoneName);
  preferences.putString("tzRule", timezoneRule);

  uint8_t dayMask = 0;
  for (int d = 0; d < 7; d++) {
    if (wateringDays[d]) {
      dayMask |= (1 << d);
    }
  }
  preferences.putUChar("days", dayMask);
  preferences.putInt("lastRun", lastRunDate);

  for (int i = 0; i < NUM_ZONES; i++) {
    String enabledKey = "z" + String(i) + "en";
    String runtimeKey = "z" + String(i) + "run";

    preferences.putBool(enabledKey.c_str(), zones[i].enabled);
    preferences.putInt(runtimeKey.c_str(), zones[i].runMinutes);
  }

  preferences.putULong64("inhibit", (uint64_t)inhibitUntil);
  preferences.end();

  Serial.println("Settings saved to NVS.");
}

// -------------------------------------------------
// NETWORK SETTINGS / AP FALLBACK
// -------------------------------------------------
void saveNetworkSettings() {
  preferences.begin("sprinkler", false);
  preferences.putString("wifiSSID", wifiSSID);
  preferences.putString("wifiPass", wifiPassword);
  preferences.putString("apPass", apPassword);
  preferences.end();

  Serial.println("Network settings saved to NVS.");
}

void connectWiFi() {
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setHostname("esp32r4-sprinkler");
  WiFi.mode(WIFI_STA);

  if (wifiSSID.length() == 0) {
    Serial.println("No WiFi SSID configured. Starting fallback AP.");
    startFallbackAP();
    return;
  }

  Serial.print("Connecting to WiFi: ");
  Serial.println(wifiSSID);
  WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());

  unsigned long startedAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedAt < WIFI_CONNECT_TIMEOUT_MS) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    apFallbackActive = false;
    wifiDisconnectedAt = 0;
    Serial.print("Connected. IP: ");
    Serial.println(WiFi.localIP());
    return;
  }

  Serial.println("WiFi connection timed out. Starting fallback AP.");
  startFallbackAP();
}

void startFallbackAP() {
  if (apPassword.length() < 8) {
    apPassword = DEFAULT_AP_PASSWORD;
  }

  WiFi.mode(WIFI_AP_STA);
  bool started = WiFi.softAP(AP_SSID, apPassword.c_str());

  if (started) {
    apFallbackActive = true;
    Serial.print("Fallback AP started. SSID: ");
    Serial.println(AP_SSID);
    Serial.print("AP IP: ");
    Serial.println(WiFi.softAPIP());
  } else {
    Serial.println("ERROR: Could not start fallback AP.");
  }

  // Keep trying the saved infrastructure network in the background.
  if (wifiSSID.length() > 0) {
    WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());
  }

  lastWiFiRetryAt = millis();
}

void handleNetwork() {
  static wl_status_t previousStatus = WL_IDLE_STATUS;
  wl_status_t status = WiFi.status();

  if (status == WL_CONNECTED) {
    wifiDisconnectedAt = 0;

    if (previousStatus != WL_CONNECTED) {
      Serial.print("WiFi connected. IP: ");
      Serial.println(WiFi.localIP());
    }

    if (apFallbackActive) {
      // Leave the fallback AP up briefly after STA recovery so an active browser
      // request can finish, then return to normal station-only operation.
      if (apRecoveryConnectedAt == 0) {
        apRecoveryConnectedAt = millis();
      } else if (millis() - apRecoveryConnectedAt >= 5000) {
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_STA);
        apFallbackActive = false;
        apRecoveryConnectedAt = 0;
        Serial.println("Fallback AP stopped after WiFi recovery.");
      }
    }
  } else {
    apRecoveryConnectedAt = 0;
    if (wifiDisconnectedAt == 0) {
      wifiDisconnectedAt = millis();
    }

    if (!apFallbackActive && millis() - wifiDisconnectedAt >= WIFI_CONNECT_TIMEOUT_MS) {
      Serial.println("WiFi has been unavailable. Enabling fallback AP.");
      startFallbackAP();
    }

    if (wifiSSID.length() > 0 && millis() - lastWiFiRetryAt >= WIFI_RETRY_INTERVAL_MS) {
      lastWiFiRetryAt = millis();
      Serial.println("Retrying saved WiFi network...");
      WiFi.reconnect();
    }
  }

  previousStatus = status;
}

// -------------------------------------------------
// TIMEZONE
// -------------------------------------------------
void applyTimezone() {
  Serial.print("Timezone: ");
  Serial.print(timezoneName);
  Serial.print("  Rule: ");
  Serial.println(timezoneRule);

  configTzTime(timezoneRule.c_str(), "pool.ntp.org", "time.nist.gov");
}

void setTimezoneFromWeb(const String& selected) {
  if (selected == "Eastern") {
    timezoneName = "Eastern";
    timezoneRule = "EST5EDT,M3.2.0/2,M11.1.0/2";
  } else if (selected == "Central") {
    timezoneName = "Central";
    timezoneRule = "CST6CDT,M3.2.0/2,M11.1.0/2";
  } else if (selected == "Mountain") {
    timezoneName = "Mountain";
    timezoneRule = "MST7MDT,M3.2.0/2,M11.1.0/2";
  } else if (selected == "Arizona") {
    timezoneName = "Arizona";
    timezoneRule = "MST7";
  } else if (selected == "Pacific") {
    timezoneName = "Pacific";
    timezoneRule = "PST8PDT,M3.2.0/2,M11.1.0/2";
  } else if (selected == "Alaska") {
    timezoneName = "Alaska";
    timezoneRule = "AKST9AKDT,M3.2.0/2,M11.1.0/2";
  } else if (selected == "Hawaii") {
    timezoneName = "Hawaii";
    timezoneRule = "HST10";
  } else if (selected == "UTC") {
    timezoneName = "UTC";
    timezoneRule = "UTC0";
  }
}

// -------------------------------------------------
// STATUS LED
// -------------------------------------------------
void handleStatusLED() {
  static unsigned long last = 0;

  if (millis() - last < 1000) {
    return;
  }

  last = millis();
  digitalWrite(STATUS_LED, WiFi.status() == WL_CONNECTED || apFallbackActive);
}

// -------------------------------------------------
// MANUAL BUTTONS
// -------------------------------------------------
void handleButtons() {
  if (otaInProgress) {
    return;
  }

  for (int i = 0; i < NUM_ZONES; i++) {
    bool raw = digitalRead(BUTTON_PINS[i]);

    if (!btnInCooldown[i]) {
      if (raw == LOW) {
        if (zones[i].enabled && !inhibitActive && sequenceState == SEQUENCE_IDLE) {
          toggleRelay(i);
        }

        btnInCooldown[i] = true;
        btnReleaseTime[i] = 0;
      }
    } else {
      if (raw == HIGH) {
        if (btnReleaseTime[i] == 0) {
          btnReleaseTime[i] = millis();
        } else if (millis() - btnReleaseTime[i] >= RELEASE_COOLDOWN_MS) {
          btnInCooldown[i] = false;
          btnReleaseTime[i] = 0;
        }
      }
    }
  }

  // Manual runs retain the original automatic timeout behavior.
  for (int i = 0; i < NUM_ZONES; i++) {
    if (zones[i].active && zones[i].manualRun) {
      unsigned long runtime = (unsigned long)zones[i].runMinutes * 60000UL;

      if (millis() - zones[i].activateAt >= runtime) {
        zones[i].active = false;
        zones[i].manualRun = false;
        digitalWrite(RELAY_PINS[i], LOW);
        Serial.printf("Zone %d: OFF (manual timeout)\n", i + 1);
      }
    }
  }
}

bool anyOtherZoneActive(int excludeIdx) {
  for (int i = 0; i < NUM_ZONES; i++) {
    if (i == excludeIdx) {
      continue;
    }

    if (zones[i].active) {
      return true;
    }
  }

  return false;
}

void toggleRelay(int i) {
  if (zones[i].active) {
    zones[i].active = false;
    zones[i].manualRun = false;
    digitalWrite(RELAY_PINS[i], LOW);
    Serial.printf("Zone %d: OFF (manual)\n", i + 1);
    return;
  }

  if (anyOtherZoneActive(i)) {
    Serial.printf("Zone %d: BLOCKED - another zone is active\n", i + 1);
    return;
  }

  zones[i].active = true;
  zones[i].manualRun = true;
  zones[i].activateAt = millis();
  digitalWrite(RELAY_PINS[i], HIGH);
  Serial.printf("Zone %d: ON (manual)\n", i + 1);
}

// -------------------------------------------------
// SCHEDULE
// -------------------------------------------------
void handleSchedules() {
  static unsigned long last = 0;

  if (otaInProgress) {
    return;
  }

  if (millis() - last < 250) {
    return;
  }

  last = millis();

  time_t now = time(nullptr);
  if (now < 100000) {
    return;
  }

  struct tm currentTime;
  localtime_r(&now, &currentTime);

  if (sequenceState == SEQUENCE_RUNNING_ZONE) {
    if (sequenceZone >= 0 && sequenceZone < NUM_ZONES) {
      unsigned long runtime = (unsigned long)zones[sequenceZone].runMinutes * 60000UL;

      if (millis() - sequenceStateStartedAt >= runtime) {
        finishSequenceZone();
      }
    }
  } else if (sequenceState == SEQUENCE_GAP) {
    if (millis() - sequenceStateStartedAt >= ZONE_GAP_MS) {
      advanceWateringSequence();
    }
  }

  if (sequenceState != SEQUENCE_IDLE || inhibitActive) {
    return;
  }

  if (anyOtherZoneActive(-1)) {
    return;
  }

  if (!wateringDays[currentTime.tm_wday]) {
    return;
  }

  if (currentTime.tm_hour != startHour || currentTime.tm_min != startMinute) {
    return;
  }

  int today = (currentTime.tm_year + 1900) * 10000 +
              (currentTime.tm_mon + 1) * 100 +
              currentTime.tm_mday;

  if (lastRunDate == today) {
    return;
  }

  if (lastScheduleYear == currentTime.tm_year && lastScheduleDay == currentTime.tm_yday) {
    return;
  }

  lastScheduleYear = currentTime.tm_year;
  lastScheduleDay = currentTime.tm_yday;
  lastRunDate = today;
  saveSettings();
  startWateringSequence();
}

void startWateringSequence() {
  int firstZone = findNextEnabledZone(-1);

  if (firstZone < 0) {
    Serial.println("Schedule started, but no zones are enabled.");
    return;
  }

  Serial.printf("Watering sequence started at %02d:%02d\n", startHour, startMinute);
  startSequenceZone(firstZone);
}

void startSequenceZone(int zoneIndex) {
  stopAllZones();

  sequenceZone = zoneIndex;
  sequenceState = SEQUENCE_RUNNING_ZONE;
  sequenceStateStartedAt = millis();

  zones[zoneIndex].active = true;
  zones[zoneIndex].manualRun = false;
  zones[zoneIndex].activateAt = millis();
  digitalWrite(RELAY_PINS[zoneIndex], HIGH);

  Serial.printf(
    "Zone %d: ON (scheduled for %d minute%s)\n",
    zoneIndex + 1,
    zones[zoneIndex].runMinutes,
    zones[zoneIndex].runMinutes == 1 ? "" : "s"
  );
}

void finishSequenceZone() {
  if (sequenceZone >= 0 && sequenceZone < NUM_ZONES) {
    zones[sequenceZone].active = false;
    digitalWrite(RELAY_PINS[sequenceZone], LOW);
    Serial.printf("Zone %d: OFF (scheduled timeout)\n", sequenceZone + 1);
  }

  int nextZone = findNextEnabledZone(sequenceZone);

  if (nextZone < 0) {
    sequenceState = SEQUENCE_IDLE;
    sequenceZone = -1;
    sequenceStateStartedAt = 0;
    Serial.println("Watering sequence complete.");
    return;
  }

  sequenceState = SEQUENCE_GAP;
  sequenceStateStartedAt = millis();
  Serial.printf("Waiting 10 seconds before Zone %d.\n", nextZone + 1);
}

void advanceWateringSequence() {
  int nextZone = findNextEnabledZone(sequenceZone);

  if (nextZone < 0) {
    sequenceState = SEQUENCE_IDLE;
    sequenceZone = -1;
    sequenceStateStartedAt = 0;
    Serial.println("Watering sequence complete.");
    return;
  }

  startSequenceZone(nextZone);
}

int findNextEnabledZone(int afterZone) {
  for (int i = afterZone + 1; i < NUM_ZONES; i++) {
    if (zones[i].enabled && zones[i].runMinutes > 0) {
      return i;
    }
  }

  return -1;
}

void cancelWateringSequence() {
  stopAllZones();
  sequenceState = SEQUENCE_IDLE;
  sequenceZone = -1;
  sequenceStateStartedAt = 0;
}

void stopAllZones() {
  for (int i = 0; i < NUM_ZONES; i++) {
    zones[i].active = false;
    zones[i].manualRun = false;
    digitalWrite(RELAY_PINS[i], LOW);
  }
}

// -------------------------------------------------
// INHIBIT
// -------------------------------------------------
void handleInhibitExpiry() {
  time_t now = time(nullptr);
  if (now < 100000) {
    return;
  }

  if (!inhibitActive && inhibitUntil > now) {
    inhibitActive = true;
    Serial.println("Restored active watering inhibit after restart.");
    return;
  }

  if (inhibitUntil != 0 && now >= inhibitUntil) {
    inhibitActive = false;
    inhibitUntil = 0;
    saveSettings();
    Serial.println("Inhibit expired.");
  }
}

// -------------------------------------------------
// HELPERS
// -------------------------------------------------
String formatTime(time_t ts) {
  if (ts < 100000) {
    return "Waiting for network time...";
  }

  char buf[32];
  struct tm localTime;
  localtime_r(&ts, &localTime);
  strftime(buf, sizeof(buf), "%a %b %d, %Y  %I:%M:%S %p", &localTime);
  return String(buf);
}

String formatStartTime();

String formatCountdown(unsigned long remainingMs) {
  unsigned long totalSeconds = (remainingMs + 999UL) / 1000UL;
  unsigned long minutes = totalSeconds / 60UL;
  unsigned long seconds = totalSeconds % 60UL;

  char buf[20];
  snprintf(buf, sizeof(buf), "%lu:%02lu", minutes, seconds);
  return String(buf);
}

String formatNextScheduledStart(time_t now) {
  if (now < 100000) {
    return "Waiting for network time...";
  }

  bool anyDaySelected = false;
  for (int d = 0; d < 7; d++) {
    if (wateringDays[d]) {
      anyDaySelected = true;
      break;
    }
  }

  if (!anyDaySelected) {
    return "No watering days selected";
  }

  struct tm currentTime;
  localtime_r(&now, &currentTime);

  int today = (currentTime.tm_year + 1900) * 10000 +
              (currentTime.tm_mon + 1) * 100 +
              currentTime.tm_mday;

  int currentSeconds = (currentTime.tm_hour * 3600) +
                       (currentTime.tm_min * 60) +
                       currentTime.tm_sec;
  int startSeconds = (startHour * 3600) + (startMinute * 60);

  for (int offset = 0; offset <= 7; offset++) {
    struct tm candidate = currentTime;
    candidate.tm_hour = startHour;
    candidate.tm_min = startMinute;
    candidate.tm_sec = 0;
    candidate.tm_mday += offset;
    candidate.tm_isdst = -1;

    time_t candidateTime = mktime(&candidate);
    if (candidateTime < 0) {
      continue;
    }

    struct tm normalized;
    localtime_r(&candidateTime, &normalized);

    if (!wateringDays[normalized.tm_wday]) {
      continue;
    }

    if (offset == 0) {
      if (lastRunDate == today || sequenceState != SEQUENCE_IDLE || currentSeconds >= startSeconds) {
        continue;
      }
    }

    char buf[80];
    strftime(buf, sizeof(buf), "%A, %B %d, %Y", &normalized);

    return String(buf);
  }

  return "No upcoming watering day found";
}

String formatStartTime() {
  char buf[16];
  int displayHour = startHour % 12;

  if (displayHour == 0) {
    displayHour = 12;
  }

  snprintf(
    buf,
    sizeof(buf),
    "%d:%02d %s",
    displayHour,
    startMinute,
    startHour >= 12 ? "PM" : "AM"
  );

  return String(buf);
}

String selectedIf(const String& value) {
  return timezoneName == value ? " selected" : "";
}

String htmlHead(const char* title) {
  String s;

  s += "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  s += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  s += "<title>";
  s += title;
  s += "</title><style>";

  s += ":root{--bg:#071a17;--panel:#0d2923;--panel2:#11372f;--line:#205448;--text:#eef8f4;--muted:#9ec0b5;--accent:#66d69e;--accent2:#8ee8b9;--danger:#ef6b73;--off:#71877f;}";
  s += "*{box-sizing:border-box;}";
  s += "body{font-family:Arial,Helvetica,sans-serif;max-width:860px;margin:0 auto;padding:24px 18px 48px;background:linear-gradient(180deg,#061511,#0a211c);color:var(--text);}";
  s += "h1{font-size:2rem;margin:0 0 8px;}h2{font-size:1.15rem;margin:0 0 14px;}";
  s += ".subtitle{color:var(--muted);margin:0 0 22px;}";
  s += ".card{background:var(--panel);border:1px solid var(--line);border-radius:14px;padding:18px;margin:14px 0;box-shadow:0 8px 24px rgba(0,0,0,.18);}";
  s += ".card-soft{background:var(--panel2);}";
  s += ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(190px,1fr));gap:12px;}";
  s += ".zone{padding:16px;border:1px solid var(--line);border-radius:12px;background:var(--panel2);}";
  s += ".zone-title{display:flex;justify-content:space-between;align-items:center;font-weight:bold;margin-bottom:10px;}";
  s += ".pill{display:inline-block;padding:5px 9px;border-radius:999px;font-size:.78rem;font-weight:bold;}";
  s += ".pill-on{background:rgba(102,214,158,.16);color:var(--accent2);border:1px solid rgba(102,214,158,.4);}";
  s += ".pill-running{background:#19e873;color:#03210f;border:1px solid #62ff9f;box-shadow:0 0 10px rgba(25,232,115,.75);}";
  s += ".pill-off{background:rgba(113,135,127,.14);color:#b7c6c1;border:1px solid rgba(113,135,127,.35);}";
  s += ".pill-danger{background:rgba(239,107,115,.16);color:#ffadb2;border:1px solid rgba(239,107,115,.45);}";
  s += ".metric{font-size:1.55rem;font-weight:bold;margin:4px 0;}";
  s += ".muted{color:var(--muted);font-size:.92rem;}";
  s += ".btn{display:inline-block;padding:11px 16px;margin:4px 6px 4px 0;border-radius:9px;cursor:pointer;border:0;text-decoration:none;font-weight:bold;font-size:.95rem;}";
  s += ".btn-primary{background:var(--accent);color:#062018;}";
  s += ".btn-danger{background:var(--danger);color:white;}";
  s += ".btn-zone{width:100%;margin-top:12px;background:#183f36;color:var(--text);border:1px solid var(--line);}";
  s += ".btn-zone-active{width:100%;margin-top:12px;background:#19e873;color:#03210f;border:1px solid #62ff9f;box-shadow:0 0 10px rgba(25,232,115,.55);}";
  s += ".btn:disabled{opacity:.42;cursor:not-allowed;box-shadow:none;}";
  s += ".controlrow{display:flex;gap:10px;align-items:center;flex-wrap:wrap;}";
  s += ".btn-secondary{background:#183f36;color:var(--text);border:1px solid var(--line);}";
  s += "label{display:block;color:var(--muted);font-size:.88rem;margin-bottom:6px;}";
  s += "input[type=number],input[type=time],input[type=text],input[type=password],input[type=file],select{width:100%;background:#071d18;color:var(--text);border:1px solid var(--line);border-radius:8px;padding:10px;font-size:1rem;}";
  s += ".days{display:grid;grid-template-columns:repeat(7,1fr);gap:6px;}";
  s += ".day{position:relative;} .day input{position:absolute;opacity:0;pointer-events:none;}";
  s += ".day span{display:block;text-align:center;padding:10px 2px;border-radius:8px;background:#071d18;border:1px solid var(--line);color:var(--muted);cursor:pointer;}";
  s += ".day input:checked+span{background:var(--accent);color:#062018;border-color:var(--accent);font-weight:bold;}";
  s += ".switchrow{display:flex;align-items:center;gap:9px;color:var(--text);font-weight:bold;}";
  s += ".switchrow input{width:18px;height:18px;}";
  s += ".footerlink{color:var(--accent2);text-decoration:none;font-weight:bold;}";
  s += "@media(max-width:560px){body{padding:16px 12px 36px}.days{gap:4px}.day span{font-size:.78rem;padding:9px 1px}}";
  s += "</style></head><body>";

  return s;
}

// -------------------------------------------------
// WEB HANDLERS
// -------------------------------------------------
void handleRoot() {
  time_t now = time(nullptr);

  // Restore persisted inhibit state after SNTP becomes available.
  if (now >= 100000 && inhibitUntil > now) {
    inhibitActive = true;
  } else if (now >= 100000 && inhibitUntil != 0 && inhibitUntil <= now) {
    inhibitUntil = 0;
    inhibitActive = false;
    saveSettings();
  }

  String html = htmlHead("Sprinkler Controller");

  html += "<h1>ESP32 Sprinkler Controller</h1>";
  html += "<p class='subtitle'>Because Mother Nature Cant Be Trusted</p>";

  html += "<div class='card'><div class='grid'>";

  html += "<div><div class='muted'>Controller Time</div>";
  html += "<div id='clock' class='metric'>" + formatTime(now) + "</div>";
  html += "<div class='muted'>Timezone: " + timezoneName + "</div></div>";

  html += "<div><div class='muted'>Watering Start Time</div>";
  html += "<div class='metric'>" + formatStartTime() + "</div>";
  html += "<div id='nextScheduled' class='muted'>" + formatNextScheduledStart(now) + "</div></div>";

  html += "<div><div class='muted'>Network</div>";
  if (WiFi.status() == WL_CONNECTED) {
    html += "<div class='metric'><span id='networkStatus' class='pill pill-on'>CONNECTED</span></div>";
    html += "<div id='networkInfo' class='muted'>" + wifiSSID + " &bull; " + WiFi.localIP().toString() + "</div>";
  } else if (apFallbackActive) {
    html += "<div class='metric'><span id='networkStatus' class='pill pill-danger'>AP MODE</span></div>";
    html += "<div id='networkInfo' class='muted'>" + String(AP_SSID) + " &bull; " + WiFi.softAPIP().toString() + "</div>";
  } else {
    html += "<div class='metric'><span id='networkStatus' class='pill pill-danger'>OFFLINE</span></div>";
    html += "<div id='networkInfo' class='muted'>Trying saved WiFi</div>";
  }
  html += "</div>";

  html += "</div></div>";

  html += "<div id='inhibitCard' class='card'><h2>Watering Controls</h2>";
  html += "<div id='inhibitContent'>";
  html += "<p id='inhibitText'></p>";
  html += "<div class='controlrow'>";
  html += "<a id='inhibitButton' class='btn' href='#'></a>";
  html += "<button id='stopSprinkler' class='btn btn-danger' type='button' onclick='stopSprinkler()'>Stop Sprinkler</button>";
  html += "</div></div></div>";

  html += "<div class='card'><h2>Zones</h2><div class='grid'>";

  for (int i = 0; i < NUM_ZONES; i++) {
    html += "<div id='zone" + String(i) + "' class='zone'>";
    html += "<div class='zone-title'><span>Zone " + String(i + 1) + "</span>";

    if (zones[i].active) {
      html += "<span id='zoneStatus" + String(i) + "' class='pill pill-running'>RUNNING</span>";
    } else if (zones[i].enabled) {
      html += "<span id='zoneStatus" + String(i) + "' class='pill pill-off'>READY</span>";
    } else {
      html += "<span id='zoneStatus" + String(i) + "' class='pill pill-off'>DISABLED</span>";
    }

    html += "</div>";
    html += "<div class='metric'>" + String(zones[i].runMinutes) + " min</div>";
    html += "<div class='muted'>Scheduled run time</div>";
    html += "<button id='zoneButton" + String(i) + "' class='btn btn-zone' type='button' onclick='toggleZone(" + String(i) + ")'>Start Zone</button>";
    html += "</div>";
  }

  html += "</div></div>";

  html += "<div id='sequenceCard' class='card card-soft' style='display:none'><strong id='sequenceText'></strong></div>";

  html += "<a class='btn btn-primary' href='/config'>Configure Watering</a>";
  html += "<a class='btn btn-secondary' href='/network'>Network / Firmware</a>";
  html += "</body>";
  html += "<script>";
  html += "var statusTimer=null;";
  html += "function scheduleStatusUpdate(){if(statusTimer!==null){clearTimeout(statusTimer);}statusTimer=setTimeout(updateStatus,1000);}";
  html += "function updateStatus(){fetch('/status?t='+Date.now(),{cache:'no-store'}).then(function(r){if(!r.ok){throw new Error('status');}return r.json();}).then(function(s){";
  html += "document.getElementById('clock').textContent=s.time;";
  html += "document.getElementById('nextScheduled').textContent=s.nextScheduled;";
  html += "var ns=document.getElementById('networkStatus');var ni=document.getElementById('networkInfo');ns.textContent=s.networkStatus;ns.className='pill '+(s.networkConnected?'pill-on':'pill-danger');ni.textContent=s.networkInfo;";
  html += "for(var i=0;i<4;i++){var z=s.zones[i];var p=document.getElementById('zoneStatus'+i);var b=document.getElementById('zoneButton'+i);p.textContent=z.status;p.className='pill '+(z.active?'pill-running':'pill-off');if(z.active&&z.manualRun){b.textContent='Stop Zone';b.className='btn btn-zone-active';}else{b.textContent='Start Zone';b.className='btn btn-zone';}b.disabled=!z.enabled||s.inhibitActive||s.sequenceRunning||(s.manualActiveZone>=0&&s.manualActiveZone!==i);}";
  html += "var sc=document.getElementById('sequenceCard');var st=document.getElementById('sequenceText');if(s.sequenceText){st.textContent=s.sequenceText;sc.style.display='block';}else{sc.style.display='none';}";
  html += "var it=document.getElementById('inhibitText');var ib=document.getElementById('inhibitButton');if(s.inhibitActive){it.className='';it.textContent='Watering is inhibited for another '+s.inhibitRemaining+'.';ib.textContent='Release Inhibit';ib.className='btn btn-primary';ib.href='/inhibit?release=1';ib.onclick=null;}else{it.className='muted';it.textContent='Temporarily stop all scheduled and manual watering for 24 hours.';ib.textContent='Activate 24 Hour Inhibit';ib.className='btn btn-danger';ib.href='/inhibit';ib.onclick=function(){return confirm('Inhibit all watering for 24 hours?');}}";
  html += "var sb=document.getElementById('stopSprinkler');sb.disabled=!(s.sequenceRunning||s.manualActiveZone>=0);";
  html += "}).catch(function(){console.log('Status update failed');}).then(function(){scheduleStatusUpdate();});}";
  html += "function toggleZone(i){fetch('/zone',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'zone='+encodeURIComponent(i)}).then(function(){updateStatus();}).catch(function(){updateStatus();});}";
  html += "function stopSprinkler(){if(!confirm('Stop all sprinkler watering now?')){return;}fetch('/stop',{method:'POST'}).then(function(){updateStatus();}).catch(function(){updateStatus();});}";
  html += "window.addEventListener('load',function(){updateStatus();});";
  html += "window.addEventListener('focus',function(){updateStatus();});";
  html += "document.addEventListener('visibilitychange',function(){if(!document.hidden){updateStatus();}});";
  html += "</script></html>";

  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "0");
  server.send(200, "text/html", html);
}

void handleConfig() {
  String html = htmlHead("Configure Sprinkler");

  const char* dayNames[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

  char startValue[8];
  snprintf(startValue, sizeof(startValue), "%02d:%02d", startHour, startMinute);

  html += "<h1>Watering Setup</h1>";
  html += "<p class='subtitle'>The schedule starts once, then each enabled zone runs in order with a 10 second gap between zones.</p>";
  html += "<form method='POST' action='/save'>";

  html += "<div class='card'><h2>Schedule</h2>";
  html += "<div class='grid'>";
  html += "<div><label for='start'>Start time</label><input id='start' type='time' name='start' value='";
  html += startValue;
  html += "' required></div>";

  html += "<div><label for='timezone'>Timezone</label><select id='timezone' name='timezone'>";
  html += "<option value='Eastern'" + selectedIf("Eastern") + ">US Eastern</option>";
  html += "<option value='Central'" + selectedIf("Central") + ">US Central</option>";
  html += "<option value='Mountain'" + selectedIf("Mountain") + ">US Mountain</option>";
  html += "<option value='Arizona'" + selectedIf("Arizona") + ">Arizona</option>";
  html += "<option value='Pacific'" + selectedIf("Pacific") + ">US Pacific</option>";
  html += "<option value='Alaska'" + selectedIf("Alaska") + ">Alaska</option>";
  html += "<option value='Hawaii'" + selectedIf("Hawaii") + ">Hawaii</option>";
  html += "<option value='UTC'" + selectedIf("UTC") + ">UTC</option>";
  html += "</select></div>";
  html += "</div>";

  html += "<label style='margin-top:16px'>Watering days</label><div class='days'>";
  for (int d = 0; d < 7; d++) {
    html += "<label class='day'><input type='checkbox' name='day" + String(d) + "'";
    if (wateringDays[d]) {
      html += " checked";
    }
    html += "><span>" + String(dayNames[d]) + "</span></label>";
  }
  html += "</div>";
  html += "</div>";

  html += "<div class='card'><h2>Zone Run Times</h2><div class='grid'>";

  for (int i = 0; i < NUM_ZONES; i++) {
    html += "<div class='zone'>";
    html += "<div class='zone-title'><span>Zone " + String(i + 1) + "</span></div>";
    html += "<label class='switchrow'><input type='checkbox' name='z" + String(i) + "en'";
    if (zones[i].enabled) {
      html += " checked";
    }
    html += "> Enabled</label>";
    html += "<label style='margin-top:14px' for='z" + String(i) + "run'>Run time in minutes</label>";
    html += "<input id='z" + String(i) + "run' type='number' name='z" + String(i) + "run' min='1' max='1440' value='" + String(zones[i].runMinutes) + "'>";
    html += "</div>";
  }

  html += "</div></div>";
  html += "<button type='submit' class='btn btn-primary'>Save Settings</button>";
  html += "<a href='/' class='btn btn-secondary'>Cancel</a>";
  html += "</form></body></html>";

  server.send(200, "text/html", html);
}

void handleSave() {
  if (server.hasArg("start")) {
    String start = server.arg("start");
    int colon = start.indexOf(':');

    if (colon > 0) {
      int newHour = start.substring(0, colon).toInt();
      int newMinute = start.substring(colon + 1).toInt();

      if (newHour >= 0 && newHour <= 23 && newMinute >= 0 && newMinute <= 59) {
        startHour = newHour;
        startMinute = newMinute;
      }
    }
  }

  for (int d = 0; d < 7; d++) {
    wateringDays[d] = server.hasArg("day" + String(d));
  }

  for (int i = 0; i < NUM_ZONES; i++) {
    zones[i].enabled = server.hasArg("z" + String(i) + "en");

    String runtimeKey = "z" + String(i) + "run";
    if (server.hasArg(runtimeKey)) {
      int value = server.arg(runtimeKey).toInt();

      if (value < 1) {
        value = 1;
      }

      if (value > 1440) {
        value = 1440;
      }

      zones[i].runMinutes = value;
    }
  }

  if (server.hasArg("timezone")) {
    setTimezoneFromWeb(server.arg("timezone"));
  }

  saveSettings();
  applyTimezone();

  Serial.println("Configuration updated from web page.");
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleTime() {
  server.send(200, "text/plain", formatTime(time(nullptr)));
}

void handleStatus() {
  time_t now = time(nullptr);

  String json = "{";
  json += "\"time\":\"" + formatTime(now) + "\",";
  json += "\"nextScheduled\":\"" + formatNextScheduledStart(now) + "\",";
  json += "\"inhibitActive\":" + String(inhibitActive ? "true" : "false") + ",";
  json += "\"sequenceRunning\":" + String(sequenceState != SEQUENCE_IDLE ? "true" : "false") + ",";


  bool networkConnected = WiFi.status() == WL_CONNECTED;
  String networkStatus = networkConnected ? "CONNECTED" : (apFallbackActive ? "AP MODE" : "OFFLINE");
  String networkInfo;
  if (networkConnected) {
    networkInfo = wifiSSID + " - " + WiFi.localIP().toString();
  } else if (apFallbackActive) {
    networkInfo = String(AP_SSID) + " - " + WiFi.softAPIP().toString();
  } else {
    networkInfo = "Trying saved WiFi";
  }
  json += "\"networkConnected\":" + String(networkConnected ? "true" : "false") + ",";
  json += "\"networkStatus\":\"" + networkStatus + "\",";
  json += "\"networkInfo\":\"" + networkInfo + "\",";

  int manualActiveZone = -1;
  for (int i = 0; i < NUM_ZONES; i++) {
    if (zones[i].active && zones[i].manualRun) {
      manualActiveZone = i;
      break;
    }
  }
  json += "\"manualActiveZone\":" + String(manualActiveZone) + ",";

  long inhibitSeconds = 0;
  if (inhibitActive && inhibitUntil > now) {
    inhibitSeconds = (long)(inhibitUntil - now);
  }

  String inhibitRemaining = String(inhibitSeconds / 3600) + "h " +
                            String((inhibitSeconds % 3600) / 60) + "m " +
                            String(inhibitSeconds % 60) + "s";
  json += "\"inhibitRemaining\":\"" + inhibitRemaining + "\",";

  String sequenceText = "";
  if (sequenceState == SEQUENCE_RUNNING_ZONE && sequenceZone >= 0 && sequenceZone < NUM_ZONES) {
    unsigned long runtime = (unsigned long)zones[sequenceZone].runMinutes * 60000UL;
    unsigned long elapsed = millis() - sequenceStateStartedAt;
    unsigned long remaining = elapsed >= runtime ? 0 : runtime - elapsed;
    sequenceText = "Scheduled watering is running Zone " + String(sequenceZone + 1) +
                   ". " + formatCountdown(remaining) + " remaining.";
  } else if (sequenceState == SEQUENCE_GAP) {
    unsigned long elapsed = millis() - sequenceStateStartedAt;
    unsigned long remaining = elapsed >= ZONE_GAP_MS ? 0 : ZONE_GAP_MS - elapsed;
    int nextZone = findNextEnabledZone(sequenceZone);
    sequenceText = "Pressure stabilization delay";
    if (nextZone >= 0) {
      sequenceText += " before Zone " + String(nextZone + 1);
    }
    sequenceText += ". " + formatCountdown(remaining) + " remaining.";
  }

  json += "\"sequenceText\":\"" + sequenceText + "\",";
  json += "\"zones\":[";

  for (int i = 0; i < NUM_ZONES; i++) {
    if (i > 0) {
      json += ",";
    }

    String status;
    if (zones[i].active) {
      unsigned long runtime = (unsigned long)zones[i].runMinutes * 60000UL;
      unsigned long elapsed = millis() - zones[i].activateAt;
      unsigned long remaining = elapsed >= runtime ? 0 : runtime - elapsed;
      status = "RUNNING " + formatCountdown(remaining);
    } else if (zones[i].enabled) {
      status = "READY";
    } else {
      status = "DISABLED";
    }

    json += "{\"active\":" + String(zones[i].active ? "true" : "false") +
            ",\"manualRun\":" + String(zones[i].manualRun ? "true" : "false") +
            ",\"enabled\":" + String(zones[i].enabled ? "true" : "false") +
            ",\"status\":\"" + status + "\"}";
  }

  json += "]}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

void handleInhibit() {
  if (server.hasArg("release")) {
    inhibitActive = false;
    inhibitUntil = 0;
    saveSettings();

    Serial.println("Inhibit released.");
    server.sendHeader("Location", "/");
    server.send(303);
    return;
  }

  time_t now = time(nullptr);
  if (now < 100000) {
    server.send(500, "text/plain", "Time has not synchronized yet. Try again shortly.");
    return;
  }

  cancelWateringSequence();
  inhibitActive = true;
  inhibitUntil = now + (24 * 3600);
  saveSettings();

  Serial.printf("Watering inhibited until %s\n", formatTime(inhibitUntil).c_str());

  server.sendHeader("Location", "/");
  server.send(303);
}

void handleZoneToggle() {
  if (!server.hasArg("zone")) {
    server.send(400, "application/json", "{\"ok\":false,\"message\":\"Missing zone\"}");
    return;
  }

  int zoneIndex = server.arg("zone").toInt();
  if (zoneIndex < 0 || zoneIndex >= NUM_ZONES) {
    server.send(400, "application/json", "{\"ok\":false,\"message\":\"Invalid zone\"}");
    return;
  }

  if (otaInProgress) {
    server.send(409, "application/json", "{\"ok\":false,\"message\":\"Firmware update is running\"}");
    return;
  }

  if (sequenceState != SEQUENCE_IDLE) {
    server.send(409, "application/json", "{\"ok\":false,\"message\":\"Scheduled watering is running\"}");
    return;
  }

  if (inhibitActive) {
    server.send(409, "application/json", "{\"ok\":false,\"message\":\"Watering is inhibited\"}");
    return;
  }

  if (!zones[zoneIndex].enabled) {
    server.send(409, "application/json", "{\"ok\":false,\"message\":\"Zone is disabled\"}");
    return;
  }

  // The active zone may always toggle itself off. Starting another zone while
  // a manual zone is already running is blocked, matching the physical buttons.
  if (!zones[zoneIndex].active && anyOtherZoneActive(zoneIndex)) {
    server.send(409, "application/json", "{\"ok\":false,\"message\":\"Another zone is active\"}");
    return;
  }

  toggleRelay(zoneIndex);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleStopSprinkler() {
  bool hadWatering = sequenceState != SEQUENCE_IDLE;

  for (int i = 0; i < NUM_ZONES; i++) {
    if (zones[i].active) {
      hadWatering = true;
      break;
    }
  }

  cancelWateringSequence();

  if (hadWatering) {
    Serial.println("All sprinkler watering stopped from web page.");
  }

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleNetworkPage() {
  String html = htmlHead("Network / Firmware");

  html += "<h1>Network / Firmware</h1>";
  html += "<p class='subtitle'>WiFi recovery and firmware maintenance. Watering settings remain stored separately in NVS.</p>";

  html += "<div class='card'><h2>Current Network</h2>";
  if (WiFi.status() == WL_CONNECTED) {
    html += "<div class='metric'><span class='pill pill-on'>CONNECTED</span></div>";
    html += "<p class='muted'>SSID: " + wifiSSID + "<br>IP: " + WiFi.localIP().toString() + "</p>";
  } else if (apFallbackActive) {
    html += "<div class='metric'><span class='pill pill-danger'>ACCESS POINT MODE</span></div>";
    html += "<p class='muted'>SSID: " + String(AP_SSID) + "<br>IP: " + WiFi.softAPIP().toString() + "</p>";
  } else {
    html += "<div class='metric'><span class='pill pill-danger'>OFFLINE</span></div>";
  }
  html += "</div>";

  html += "<form method='POST' action='/network/save'>";
  html += "<div class='card'><h2>WiFi Settings</h2>";
  html += "<label for='ssid'>Home WiFi SSID</label>";
  html += "<input id='ssid' type='text' name='ssid' maxlength='32' value='" + wifiSSID + "' required>";
  html += "<label style='margin-top:14px' for='wifiPass'>Home WiFi password</label>";
  html += "<input id='wifiPass' type='password' name='wifiPass' maxlength='64' placeholder='Leave blank to keep current password'>";
  html += "<p class='muted'>After saving, the controller restarts and tries this network. If it cannot connect within 20 seconds it starts its own access point automatically.</p>";
  html += "</div>";

  html += "<div class='card'><h2>Fallback Access Point</h2>";
  html += "<p class='muted'>SSID: <strong>" + String(AP_SSID) + "</strong><br>Default address: <strong>192.168.4.1</strong></p>";
  html += "<label for='apPass'>AP password</label>";
  html += "<input id='apPass' type='password' name='apPass' minlength='8' maxlength='63' placeholder='Leave blank to keep current password'>";
  html += "</div>";

  html += "<button type='submit' class='btn btn-primary'>Save Network Settings</button>";
  html += "<a href='/' class='btn btn-secondary'>Cancel</a>";
  html += "</form>";

  html += "<div class='card card-soft'><h2>Firmware</h2>";
  html += "<div class='metric'>Version " + String(FIRMWARE_VERSION) + "</div>";
  html += "<p class='muted'>Upload a compiled ESP32 firmware .bin file. All sprinkler relays are turned off before an update begins.</p>";
  html += "<a class='btn btn-primary' href='/update'>Update Firmware</a>";
  html += "</div>";
  html += "</body></html>";

  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.send(200, "text/html", html);
}

void handleSaveNetwork() {
  if (!server.hasArg("ssid")) {
    server.send(400, "text/plain", "WiFi SSID is required.");
    return;
  }

  String newSSID = server.arg("ssid");
  newSSID.trim();
  if (newSSID.length() == 0 || newSSID.length() > 32) {
    server.send(400, "text/plain", "WiFi SSID must be 1 to 32 characters.");
    return;
  }

  wifiSSID = newSSID;

  if (server.hasArg("wifiPass") && server.arg("wifiPass").length() > 0) {
    wifiPassword = server.arg("wifiPass");
  }

  if (server.hasArg("apPass") && server.arg("apPass").length() > 0) {
    String newApPassword = server.arg("apPass");
    if (newApPassword.length() < 8 || newApPassword.length() > 63) {
      server.send(400, "text/plain", "AP password must be 8 to 63 characters.");
      return;
    }
    apPassword = newApPassword;
  }

  saveNetworkSettings();

  String html = htmlHead("Network Settings Saved");
  html += "<h1>Network Settings Saved</h1>";
  html += "<div class='card'><p>The controller is restarting and will try <strong>" + wifiSSID + "</strong>.</p>";
  html += "<p class='muted'>If it cannot connect, join <strong>" + String(AP_SSID) + "</strong> and open <strong>192.168.4.1</strong>.</p></div>";
  html += "</body></html>";
  server.send(200, "text/html", html);
  delay(1000);
  ESP.restart();
}

void handleUpdatePage() {
  String html = htmlHead("Firmware Update");
  html += "<h1>Firmware Update</h1>";
  html += "<p class='subtitle'>Current version: " + String(FIRMWARE_VERSION) + "</p>";
  html += "<div class='card'><h2>Upload Firmware</h2>";
  html += "<p class='muted'>Select the compiled .bin file. Starting an update immediately turns off all sprinkler valves.</p>";
  html += "<form id='uploadForm'>";
  html += "<input id='firmware' type='file' name='update' accept='.bin,application/octet-stream' required>";
  html += "<button type='submit' class='btn btn-primary' style='margin-top:14px'>Upload and Update</button>";
  html += "<a href='/network' class='btn btn-secondary'>Cancel</a>";
  html += "</form>";
  html += "<div id='progressWrap' style='display:none;margin-top:18px'>";
  html += "<div class='muted' id='progressText'>Preparing update...</div>";
  html += "<div style='height:14px;background:#071d18;border:1px solid var(--line);border-radius:999px;overflow:hidden;margin-top:8px'>";
  html += "<div id='progressBar' style='height:100%;width:0;background:var(--accent)'></div></div></div>";
  html += "<div id='result' style='margin-top:16px'></div>";
  html += "</div>";
  html += "<script>";
  html += "document.getElementById('uploadForm').addEventListener('submit',function(e){e.preventDefault();var f=document.getElementById('firmware').files[0];if(!f){return;}if(!confirm('Update firmware now? All watering will stop during the update.')){return;}var fd=new FormData();fd.append('update',f);var x=new XMLHttpRequest();document.getElementById('progressWrap').style.display='block';x.upload.onprogress=function(ev){if(ev.lengthComputable){var p=Math.round((ev.loaded/ev.total)*100);document.getElementById('progressBar').style.width=p+'%';document.getElementById('progressText').textContent='Uploading firmware... '+p+'%';}};x.onload=function(){document.getElementById('result').innerHTML=x.responseText;if(x.status===200){document.getElementById('progressText').textContent='Update complete. Controller is restarting...';}};x.onerror=function(){document.getElementById('result').textContent='Firmware upload failed.';};x.open('POST','/update',true);x.send(fd);});";
  html += "</script></body></html>";

  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.send(200, "text/html", html);
}

void handleFirmwareUpload() {
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("OTA update starting: %s\n", upload.filename.c_str());
    otaInProgress = true;
    otaUploadStarted = true;
    otaUpdateSuccess = false;
    cancelWateringSequence();

    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      otaUpdateSuccess = true;
      Serial.printf("OTA update complete: %u bytes.\n", upload.totalSize);
    } else {
      Update.printError(Serial);
      otaUpdateSuccess = false;
      otaInProgress = false;
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.end();
    otaUpdateSuccess = false;
    otaInProgress = false;
    Serial.println("OTA update aborted.");
  }
}

void handleFirmwareUpdate() {
  if (!otaUploadStarted || !otaUpdateSuccess || Update.hasError()) {
    otaInProgress = false;
    server.send(500, "text/html", "<strong>Firmware update failed.</strong> The existing firmware is still active.");
    return;
  }

  server.send(200, "text/html", "<strong>Firmware update successful.</strong> Controller restarting...");
  delay(800);
  ESP.restart();
}

void handle404() {
  server.send(404, "text/plain", "Not Found");
}
