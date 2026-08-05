// console.cpp - see console.h.

#include "console.h"

#include "config.h"
#include "display.h"
#include "identity.h"
#include "menus.h"
#include "modes.h"
#include "mqtt.h"
#include "net.h"
#include "store.h"
#include "ui.h"

#if DEBUG_SERIAL

static char line[160];
static uint8_t len = 0;

static void showConfig() {
  Serial.printf("badge   id=%s secret=%s v=%s\n", badgeId, badgeCode,
                BADGE_VERSION);
  Serial.printf("name    %s\n", nametagName);
  Serial.printf("wifi    ssid=%s pass=%s state=%s\n",
                wifiSsid[0] ? wifiSsid : "(unset)",
                wifiPass[0] ? "(set)" : "(unset)", wifiStateText());
  if (wifiIsConnected()) Serial.printf("ip      %s\n", wifiIpText());
  Serial.printf("mqtt    %s:%s user=%s pass=%s topic=%s state=%s\n",
                iotBroker[0] ? iotBroker : "(unset)", iotPort,
                iotUser[0] ? iotUser : "(none)", iotPass[0] ? "(set)" : "(none)",
                iotTopic, mqttStateText());
}

// Splits on spaces in place. Returns the number of tokens found.
static int tokenize(char *s, char *tok[], int maxTok) {
  int n = 0;
  char *p = s;
  while (*p && n < maxTok) {
    while (*p == ' ') p++;
    if (!*p) break;
    tok[n++] = p;
    while (*p && *p != ' ') p++;
    if (*p) *p++ = '\0';
  }
  return n;
}

static void handle(char *s) {
  char *tok[6];
  const int n = tokenize(s, tok, 6);
  if (n == 0) return;

  for (char *c = tok[0]; *c; c++) *c = tolower(*c);

  if (!strcmp(tok[0], "help")) {
    Serial.println("wifi <ssid> <pass> | mqtt <host> <port> [user] [pw] | "
                   "name <text> | show <n> | list | status");
  } else if (!strcmp(tok[0], "status")) {
    showConfig();
  } else if (!strcmp(tok[0], "wifi") && n >= 2) {
    setField(wifiSsid, sizeof(wifiSsid), tok[1]);
    setField(wifiPass, sizeof(wifiPass), n >= 3 ? tok[2] : "");
    wifiConnect();
    saveSettings();  // after connect so wifiOn persists
    refreshSysLabels();
    Serial.printf("wifi: set to '%s', joining\n", wifiSsid);
  } else if (!strcmp(tok[0], "mqtt") && n >= 3) {
    setField(iotBroker, sizeof(iotBroker), tok[1]);
    setField(iotPort, sizeof(iotPort), tok[2]);
    if (n >= 4) setField(iotUser, sizeof(iotUser), tok[3]);
    if (n >= 5) setField(iotPass, sizeof(iotPass), tok[4]);
    saveSettings();
    refreshIotLabels();
    Serial.printf("mqtt: broker set to %s:%s\n", iotBroker, iotPort);
  } else if (!strcmp(tok[0], "show") && n >= 2) {
    const int v = atoi(tok[1]);
    if (v >= 0 && v < (int)menus[MENU_MODE].count) {
      if (modeActive) modesExit();
      updateMenuSelection(menus[MENU_MODE], (uint8_t)v);
      saveSettings();
      modeActive = true;
      modesEnter(millis());
      render();
      Serial.printf("show: %s\n", modeItems[activeMode()]);
    } else {
      Serial.printf("show: 0..%u\n", (unsigned)menus[MENU_MODE].count - 1);
    }
  } else if (!strcmp(tok[0], "list")) {
    for (uint8_t i = 0; i < menus[MENU_MODE].count; i++) {
      Serial.printf("  %u %s%s\n", (unsigned)i, modeItems[i],
                    i == activeMode() ? "  <-" : "");
    }
  } else if (!strcmp(tok[0], "name") && n >= 2) {
    // Re-join the tail so names with spaces survive tokenising.
    char joined[sizeof(nametagName)] = "";
    for (int i = 1; i < n; i++) {
      if (i > 1) strncat(joined, " ", sizeof(joined) - strlen(joined) - 1);
      strncat(joined, tok[i], sizeof(joined) - strlen(joined) - 1);
    }
    setField(nametagName, sizeof(nametagName), joined);
    saveSettings();
    render();
    Serial.printf("name: '%s'\n", nametagName);
  } else {
    Serial.println("? try: help");
  }
}

void consoleTick() {
  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      line[len] = '\0';
      if (len) handle(line);
      len = 0;
    } else if (len < sizeof(line) - 1) {
      line[len++] = c;
    }
  }
}

#else
void consoleTick() {}
#endif
