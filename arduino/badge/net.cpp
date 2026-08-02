// net.cpp - see net.h.

#include "net.h"

#include <WiFi.h>

#include "config.h"
#include "menus.h"
#include "store.h"

static uint32_t lastWifiPoll = 0;
static wl_status_t lastWifiStatus = WL_NO_SHIELD;

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
