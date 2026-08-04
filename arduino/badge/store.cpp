// store.cpp - see store.h.

#include "store.h"

#include <Preferences.h>

#include "display.h"
#include "identity.h"
#include "menus.h"
#include "modes.h"

char nametagName[24] = DEFAULT_NAME;

char iotBroker[32] = "mqtt.local";
char iotPort[6] = "1883";
char iotUser[16] = "badge";
char iotPass[48] = "";
char iotTopic[24] = "dc34";
char iotClientId[16] = "";
bool iotOnline = false;

const uint8_t BRIGHT_PCT[] = {15, 30, 50, 75, 100};
uint8_t brightness = BRIGHT_COUNT - 1;  // default full
bool showBatteryIcon = true;
bool shareWifiScans = false;

char popupText[48] = "";

bool displayFlipped = false;

// Seeded from config.h, and only a seed: loadSettings() leaves these alone when
// NVS has no stored network, so a blank badge comes up on the fleet AP and can
// be reached by OTA, while a badge someone has pointed elsewhere keeps its own.
char wifiSsid[33] = DEFAULT_WIFI_SSID;
char wifiPass[64] = DEFAULT_WIFI_PASS;
bool wifiWanted = false;

static Preferences prefs;
static bool settingsDirty = false;
static uint32_t lastChange = 0;

void setField(char *dst, size_t n, const char *src) {
  strncpy(dst, src, n - 1);
  dst[n - 1] = '\0';
}

// A client id that is stable per badge but distinct between badges, so two
// badges on the same broker do not fight over one MQTT session.
void regenerateClientId() {
  snprintf(iotClientId, sizeof(iotClientId), "dc34-%04lx",
           (unsigned long)(esp_random() & 0xFFFF));
}

void loadSettings() {
  prefs.begin("badge", true);  // read-only
  for (int i = 0; i < MENU_COUNT; i++) {
    char key[8];
    snprintf(key, sizeof(key), "m%d", i);
    uint8_t v = prefs.getUChar(key, 0);
    if (v < menus[i].count) menus[i].index = v;
  }
  prefs.getString("name", nametagName, sizeof(nametagName));
  prefs.getString("broker", iotBroker, sizeof(iotBroker));
  prefs.getString("port", iotPort, sizeof(iotPort));
  prefs.getString("user", iotUser, sizeof(iotUser));
  prefs.getString("mpw", iotPass, sizeof(iotPass));
  prefs.getString("topic", iotTopic, sizeof(iotTopic));
  prefs.getString("cid", iotClientId, sizeof(iotClientId));
  prefs.getString("ssid", wifiSsid, sizeof(wifiSsid));
  prefs.getString("wpw", wifiPass, sizeof(wifiPass));
  brightness = prefs.getUChar("bright", BRIGHT_COUNT - 1);
  showBatteryIcon = prefs.getBool("batt", true);
  shareWifiScans = prefs.getBool("scanShare", false);
  prefs.getString("popup", popupText, sizeof(popupText));
  displayFlipped = prefs.getBool("flip", false);
  if (brightness >= BRIGHT_COUNT) brightness = BRIGHT_COUNT - 1;
  prefs.end();

  // First boot on a blank NVS, or a partition wiped by a reflash.
  if (iotClientId[0] == '\0') {
    regenerateClientId();
    markDirty();
  }
  refreshIotLabels();
  refreshModeLabels();
  refreshSetLabels();
  refreshSysLabels();
  LOGF("settings loaded: mode=%s name=%s broker=%s:%s ssid=%s bright=%u%%\n",
       modeItems[activeMode()], nametagName, iotBroker, iotPort,
       wifiSsid[0] ? wifiSsid : "--", (unsigned)BRIGHT_PCT[brightness]);
}

void saveSettings() {
  prefs.begin("badge", false);
  for (int i = 0; i < MENU_COUNT; i++) {
    char key[8];
    snprintf(key, sizeof(key), "m%d", i);
    prefs.putUChar(key, menus[i].index);
  }
  prefs.putString("name", nametagName);
  prefs.putString("broker", iotBroker);
  prefs.putString("port", iotPort);
  prefs.putString("user", iotUser);
  prefs.putString("mpw", iotPass);
  prefs.putString("topic", iotTopic);
  prefs.putString("cid", iotClientId);
  prefs.putString("ssid", wifiSsid);
  prefs.putString("wpw", wifiPass);
  prefs.putUChar("bright", brightness);
  prefs.putBool("batt", showBatteryIcon);
  prefs.putBool("scanShare", shareWifiScans);
  prefs.putString("popup", popupText);
  prefs.putBool("flip", displayFlipped);
  prefs.end();
  settingsDirty = false;
  LOGF("settings saved\n");
}

// Mark state as needing a flush; storeTick() writes it once things settle.
void markDirty() {
  settingsDirty = true;
  lastChange = millis();
}

void storeTick(uint32_t now) {
  if (settingsDirty && now - lastChange >= SAVE_DEBOUNCE_MS) saveSettings();
}

void factoryReset() {
  prefs.begin("badge", false);
  prefs.clear();
  prefs.end();

  setField(nametagName, sizeof(nametagName), DEFAULT_NAME);
  setField(iotBroker, sizeof(iotBroker), "mqtt.local");
  setField(iotPort, sizeof(iotPort), "1883");
  setField(iotUser, sizeof(iotUser), "badge");
  iotPass[0] = '\0';
  setField(iotTopic, sizeof(iotTopic), "dc34");
  // Back to the fleet AP rather than to nothing. A reset badge that cannot
  // reach the network also cannot be recovered by OTA, which is the one thing
  // you want working after someone has reset a badge you are holding.
  setField(wifiSsid, sizeof(wifiSsid), DEFAULT_WIFI_SSID);
  setField(wifiPass, sizeof(wifiPass), DEFAULT_WIFI_PASS);
  wifiWanted = false;
  brightness = BRIGHT_COUNT - 1;
  showBatteryIcon = true;
  shareWifiScans = false;
  popupText[0] = '\0';
  for (int i = 0; i < MENU_COUNT; i++) menus[i].index = 0;
  regenerateClientId();

  displayApplyBrightness();
  refreshIotLabels();
  refreshModeLabels();
  refreshSetLabels();
  refreshSysLabels();
  saveSettings();
  // Identity is deliberately untouched: a factory reset is for handing the
  // badge on with your name and network cleared, not for becoming a new badge.
  LOGF("factory reset (identity %s preserved)\n", badgeId);
}
