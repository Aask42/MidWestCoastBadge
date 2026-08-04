// net.cpp - see net.h.

#include "net.h"

#include <WiFi.h>

#include "config.h"
#include "menus.h"
#include "store.h"

static uint32_t lastWifiPoll = 0;
static wl_status_t lastWifiStatus = WL_NO_SHIELD;
static bool scanRunning = false;
static bool scanPausedAssociation = false;
static uint8_t scanCount = 0;
static uint32_t scanGeneration = 0;
static char scanSsid[WIFI_SCAN_MAX][33];
static int8_t scanRssi[WIFI_SCAN_MAX];
static bool scanSecure[WIFI_SCAN_MAX];
static uint8_t scanChannel[WIFI_SCAN_MAX];
static uint8_t scanAuth[WIFI_SCAN_MAX];
static char scanBssid[WIFI_SCAN_MAX][18];

#define WIFI_SEEN_MAX 256
static uint32_t seenBssid[WIFI_SEEN_MAX];
static uint16_t seenCount = 0;

static uint32_t bssidHash(const uint8_t bssid[6]) {
  uint32_t hash = 2166136261UL;
  for (uint8_t i = 0; i < 6; i++) {
    hash ^= bssid[i];
    hash *= 16777619UL;
  }
  return hash ? hash : 1;
}

static void rememberBssid(const uint8_t bssid[6]) {
  const uint32_t hash = bssidHash(bssid);
  for (uint16_t i = 0; i < seenCount; i++) {
    if (seenBssid[i] == hash) return;
  }
  if (seenCount < WIFI_SEEN_MAX) seenBssid[seenCount++] = hash;
}

static const char *authName(uint8_t auth) {
  switch ((wifi_auth_mode_t)auth) {
    case WIFI_AUTH_OPEN: return "OPEN";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA12";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "EAP";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA23";
    default: return "SEC";
  }
}

const char *wifiStateText() {
  switch (WiFi.status()) {
    case WL_CONNECTED: return "connected";
    case WL_NO_SSID_AVAIL: return "no such network";
    case WL_CONNECT_FAILED: return "auth failed";
    case WL_CONNECTION_LOST: return "lost";
    case WL_DISCONNECTED: return "disconnected";
    case WL_IDLE_STATUS: return "idle";
    default: return wifiWanted ? "connecting" : "off";
  }
}

bool wifiIsConnected() { return WiFi.status() == WL_CONNECTED; }

const char *wifiIpText() {
  static char buf[20];
  snprintf(buf, sizeof(buf), "%s", WiFi.localIP().toString().c_str());
  return buf;
}

bool wifiScanStart() {
  if (scanRunning) return false;
  WiFi.scanDelete();
  WiFi.mode(WIFI_STA);
  scanCount = 0;
  const int result = WiFi.scanNetworks(true, true);
  scanRunning = (result == WIFI_SCAN_RUNNING);
  if (!scanRunning) LOGF("wifi scan: failed to start (%d)\n", result);
  return scanRunning;
}

void wifiScanModeEnter() {
  scanPausedAssociation = wifiWanted && !wifiIsConnected();
  if (scanPausedAssociation) {
    WiFi.disconnect(false, false);
    WiFi.mode(WIFI_STA);
    LOGF("wifi scan: paused association\n");
  }
}

void wifiScanModeExit() {
  if (scanRunning) {
    WiFi.scanDelete();
    scanRunning = false;
  }
  if (scanPausedAssociation) {
    scanPausedAssociation = false;
    wifiConnect();
    LOGF("wifi scan: resumed association\n");
  }
}

bool wifiScanTick() {
  if (!scanRunning) return false;
  const int found = WiFi.scanComplete();
  if (found == WIFI_SCAN_RUNNING) return false;

  scanRunning = false;
  scanCount = found > 0 ? (uint8_t)min(found, WIFI_SCAN_MAX) : 0;
  for (uint8_t i = 0; i < scanCount; i++) {
    const String ssid = WiFi.SSID(i);
    snprintf(scanSsid[i], sizeof(scanSsid[i]), "%s",
             ssid.length() ? ssid.c_str() : "<hidden>");
    scanRssi[i] = (int8_t)WiFi.RSSI(i);
    scanAuth[i] = (uint8_t)WiFi.encryptionType(i);
    scanSecure[i] = scanAuth[i] != WIFI_AUTH_OPEN;
    scanChannel[i] = (uint8_t)WiFi.channel(i);
    uint8_t bssid[6] = {};
    WiFi.BSSID(i, bssid);
    rememberBssid(bssid);
    snprintf(scanBssid[i], sizeof(scanBssid[i]),
         "%02X:%02X:%02X:%02X:%02X:%02X", bssid[0], bssid[1], bssid[2],
         bssid[3], bssid[4], bssid[5]);
  }
  WiFi.scanDelete();
  scanGeneration++;
  LOGF("wifi scan: %u network(s)\n", (unsigned)scanCount);

  if (!wifiWanted && !wifiIsConnected()) WiFi.mode(WIFI_OFF);
  return true;
}

bool wifiScanRunning() { return scanRunning; }
uint8_t wifiScanCount() { return scanCount; }
const char *wifiScanSsid(uint8_t index) {
  return index < scanCount ? scanSsid[index] : "";
}
int8_t wifiScanRssi(uint8_t index) {
  return index < scanCount ? scanRssi[index] : -127;
}
bool wifiScanSecure(uint8_t index) {
  return index < scanCount && scanSecure[index];
}
uint8_t wifiScanChannel(uint8_t index) {
  return index < scanCount ? scanChannel[index] : 0;
}
const char *wifiScanBssid(uint8_t index) {
  return index < scanCount ? scanBssid[index] : "";
}
const char *wifiScanAuth(uint8_t index) {
  return index < scanCount ? authName(scanAuth[index]) : "";
}
uint16_t wifiScanSessionCount() { return seenCount; }
uint32_t wifiScanGeneration() { return scanGeneration; }

void wifiConnect() {
  if (wifiSsid[0] == '\0') {
    LOGF("wifi: no SSID set\n");
    return;
  }
  wifiWanted = true;
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid, wifiPass);
  LOGF("wifi: connecting to '%s'\n", wifiSsid);
}

void wifiDisconnect() {
  wifiWanted = false;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  LOGF("wifi: off\n");
}

void netBegin() {
  // If a network was configured on a previous boot, start joining it now.
  // Asynchronous, so this costs nothing at boot even if the AP is not there.
  if (wifiSsid[0]) {
    wifiConnect();
    refreshSysLabels();
  }
}

bool netTick(uint32_t now) {
  if (now - lastWifiPoll < WIFI_POLL_MS) return false;
  lastWifiPoll = now;

  const wl_status_t st = WiFi.status();
  if (st == lastWifiStatus) return false;
  const wl_status_t prev = lastWifiStatus;
  lastWifiStatus = st;

  // A wall clock is the whole mechanism behind slideshow sync: badges never
  // talk to each other, they just both know what time it is. Start SNTP the
  // moment we have a route, and only once per join.
  if (st == WL_CONNECTED && prev != WL_CONNECTED) {
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    LOGF("wifi: up, SNTP started for slideshow sync\n");
  }
  LOGF("wifi: %s\n", wifiStateText());
  refreshSysLabels();
  return true;
}
