// menus.cpp - see menus.h.

#include "menus.h"

#include "display.h"
#include "identity.h"
#include "input.h"
#include "keyboard.h"
#include "modes.h"
#include "mqtt.h"
#include "net.h"
#include "power.h"
#include "store.h"

// Rows that show live values are rendered into these buffers. The item
// pointers reference the buffers, never the values themselves, so they stay
// valid for the life of the program.
static char iotLabel[IOT_ROWS][40];
static const char *iotItems[IOT_ROWS] = {iotLabel[0], iotLabel[1], iotLabel[2],
                                         iotLabel[3], iotLabel[4], iotLabel[5],
                                         iotLabel[6]};

static char sysLabel[SYS_ROWS][40];
static const char *sysItems[SYS_ROWS] = {sysLabel[0], sysLabel[1], sysLabel[2],
                                         sysLabel[3], sysLabel[4], sysLabel[5],
                                         sysLabel[6]};

// Factory reset is armed by the first tap and only acts on the second. The arm
// expires on its own so a badge cannot be left one stray tap away from a wipe,
// and any navigation cancels it outright.
static uint32_t resetArmedAt = 0;
static const uint32_t RESET_ARM_MS = 8000;

static bool resetArmed() {
  return resetArmedAt && (millis() - resetArmedAt < RESET_ARM_MS);
}

void menusDisarmReset() { resetArmedAt = 0; }

// One flat list: the nametag, the slideshow, then every animation by name.
// Choosing an animation by name IS "static" - there is no second setting to go
// hunting for, which is what made the old static/scene split confusing.
// Sized for the worst case; the live row count is set in refreshModeLabels()
// once the storage partition has been scanned, because how many bitmaps exist
// is not known until runtime.
#define MODE_ROWS_MAX (MODE_SCENE_FIRST + SCENE_COUNT + IMAGE_MAX)
static char modeLabel[MODE_ROWS_MAX][40];
static const char *modeItemPtrs[MODE_ROWS_MAX];
const char **modeItems = modeItemPtrs;

void refreshModeLabels() {
  for (int i = 0; i < MODE_ROWS_MAX; i++) modeItemPtrs[i] = modeLabel[i];
  snprintf(modeLabel[MODE_NAMETAG], sizeof(modeLabel[0]), "nametag");
  snprintf(modeLabel[MODE_SLIDESHOW], sizeof(modeLabel[0]), "slideshow (all)");
  const int shows = SHOW_COUNT;
  for (int i = 0; i < shows; i++) {
    snprintf(modeLabel[MODE_SCENE_FIRST + i], sizeof(modeLabel[0]), "%s",
             showName(i));
  }
  menus[MENU_MODE].count = (uint8_t)(MODE_SCENE_FIRST + shows);
  if (menus[MENU_MODE].index >= menus[MENU_MODE].count) {
    menus[MENU_MODE].index = MODE_SLIDESHOW;
  }
}
static char setLabel[SET_ROWS][40];
static const char *settingsItems[SET_ROWS] = {setLabel[0]};

Menu menus[] = {
    {"MQTT", iotItems, IOT_ROWS, G_DOWN, 0, 0},       // bottom edge, push down
    {"SHOW", modeItemPtrs, MODE_SCENE_FIRST + SCENE_COUNT, G_UP, 0, 0},               // top edge, push back up
    {"SETTINGS", settingsItems, SET_ROWS, G_LEFT, 0, 0},   // left edge
    {"SYSTEM", sysItems, SYS_ROWS, G_RIGHT, 0, 0},    // right edge, push right
};

uint8_t activeMode() { return menus[MENU_MODE].index; }

// === Navigation history ===
#define NAV_MAX 4
static int navStack[NAV_MAX];
static uint8_t navDepth = 0;

void navPush(int screen) {
  if (navDepth < NAV_MAX) navStack[navDepth++] = screen;
}
int navPop() { return navDepth ? navStack[--navDepth] : SCREEN_HOME; }
void navClear() { navDepth = 0; }

// === Live labels ===
void refreshIotLabels() {
  snprintf(iotLabel[IOT_BROKER], sizeof(iotLabel[0]), "broker: %s",
           iotBroker[0] ? iotBroker : "--");
  snprintf(iotLabel[IOT_PORT], sizeof(iotLabel[0]), "port: %s", iotPort);
  snprintf(iotLabel[IOT_USER], sizeof(iotLabel[0]), "user: %s",
           iotUser[0] ? iotUser : "--");
  snprintf(iotLabel[IOT_TOPIC], sizeof(iotLabel[0]), "topic: %s",
           iotTopic[0] ? iotTopic : "--");
  // Shown in the clear, on purpose. This is the one value the user has to be
  // able to read off the screen and type into the web client; masking it would
  // defeat its only job. It is a short-lived pairing token, not a password -
  // see identity.h.
  snprintf(iotLabel[IOT_SECRET], sizeof(iotLabel[0]), "secret: %s", badgeCode);
  snprintf(iotLabel[IOT_STATUS], sizeof(iotLabel[0]), "status: %s",
           mqttStateText());
  // Masked in the list: unlike the pairing secret, this one is a real
  // credential and has no reason to be readable off a badge on a table.
  snprintf(iotLabel[IOT_PASS], sizeof(iotLabel[0]), "pw: %s",
           iotPass[0] ? "********" : "--");
}

void refreshModeLabels();

void refreshSetLabels() {
  snprintf(setLabel[SET_NAME], sizeof(setLabel[0]), "set name");
}

void refreshSysLabels() {
  snprintf(sysLabel[SYS_BRIGHT], sizeof(sysLabel[0]), "brightness: %u%%",
           (unsigned)BRIGHT_PCT[brightness]);
  {
    const int pct = batteryPercent();
    if (showBatteryIcon && pct >= 0) {
      snprintf(sysLabel[SYS_BATT], sizeof(sysLabel[0]), "battery: on (%d%%)",
               pct);
    } else if (showBatteryIcon) {
      // Says "no sense" rather than "0%" - there is a real difference between
      // a flat pack and a pin that is not wired to anything.
      snprintf(sysLabel[SYS_BATT], sizeof(sysLabel[0]), "battery: on (no sense)");
    } else {
      snprintf(sysLabel[SYS_BATT], sizeof(sysLabel[0]), "battery icon: off");
    }
  }
  snprintf(sysLabel[SYS_SSID], sizeof(sysLabel[0]), "wifi: %s",
           wifiSsid[0] ? wifiSsid : "--");
  // Masked in the list so a shoulder-surfer at a con does not read it off the
  // badge. Shown in clear while editing, where you need to check it.
  snprintf(sysLabel[SYS_PASS], sizeof(sysLabel[0]), "pw: %s",
           wifiPass[0] ? "********" : "--");
  if (wifiIsConnected()) {
    snprintf(sysLabel[SYS_STATUS], sizeof(sysLabel[0]), "ip: %s", wifiIpText());
  } else {
    snprintf(sysLabel[SYS_STATUS], sizeof(sysLabel[0]), "status: %s",
             wifiStateText());
  }
  snprintf(sysLabel[SYS_CONNECT], sizeof(sysLabel[0]), "%s",
           wifiWanted ? "disconnect" : "connect");
  // The armed label says what the next tap DOES, not what the row is for.
  snprintf(sysLabel[SYS_RESET], sizeof(sysLabel[0]), "%s",
           resetArmed() ? "TAP AGAIN TO WIPE" : "factory reset");
}

// === Rendering ===

// The nav bar names the exact swipe that leaves this menu, so the way out is
// never a guess.
void drawNavBar(Arduino_GFX *g, const Menu &m, int ox, int oy) {
  g->fillRect(ox, oy + NAV_Y, SCREEN_W, NAV_H, C_NAV);

  char label[24];
  snprintf(label, sizeof(label), "%s SWIPE %s", gestureArrow(m.retreat),
           gestureWord(m.retreat));

  g->setTextSize(2);
  g->setTextColor(C_NAV_FG);
  g->setCursor(ox + 8, oy + NAV_Y + (NAV_H - GLYPH_H * 2) / 2);
  g->print(label);

  char count[16];
  const uint8_t pages = (m.count + MENU_VISIBLE - 1) / MENU_VISIBLE;
  if (pages > 1) {
    snprintf(count, sizeof(count), "p%u/%u", (unsigned)m.page + 1,
             (unsigned)pages);
  } else {
    snprintf(count, sizeof(count), "%u/%u", (unsigned)m.index + 1,
             (unsigned)m.count);
  }
  g->setTextSize(1);
  g->setCursor(ox + SCREEN_W - textWidth(count, 1) - 8,
               oy + NAV_Y + (NAV_H - GLYPH_H) / 2);
  g->print(count);
}

// One menu row, highlight included. Shared by the full redraw and the
// single-row update so the two can never disagree about geometry.
// `y` is the top of the row's highlight rectangle.
void drawMenuRowAt(Arduino_GFX *g, const char *label, bool selected, int ox,
                   int y) {
  g->fillRect(ox + 6, y, SCREEN_W - 12, MENU_ROW_H - 4,
              selected ? C_ACCENT : C_BG);

  // IoT rows carry a value, so they can outrun the row at size 2; drop a size
  // rather than clip mid-word. A full-length broker name overruns even at
  // size 1, so past that point it is truncated with a marker - GFX would
  // otherwise run the text off the panel with no sign anything was lost.
  const int roomPx = SCREEN_W - 28;
  uint8_t size = 2;
  while (size > 1 && textWidth(label, size) > roomPx) size--;

  char fitted[48];
  const int maxChars = roomPx / (GLYPH_W * size);
  if ((int)strlen(label) > maxChars) {
    snprintf(fitted, sizeof(fitted), "%.*s~", maxChars - 1, label);
    label = fitted;
  }

  g->setTextSize(size);
  g->setTextColor(selected ? C_BG : C_FG);
  g->setCursor(ox + 14, y + (MENU_ROW_H - 4 - GLYPH_H * size) / 2);
  g->print(label);
}

// Top of screen slot `i`'s highlight rectangle. Slots are positions on the
// panel; rows are entries in the menu. They differ once a menu paginates.
int menuRowY(uint8_t i) { return MENU_TOP + i * MENU_ROW_H; }

int menuRowAtSlot(const Menu &m, int slot) {
  if (slot < 0 || slot >= (int)MENU_VISIBLE) return -1;
  const int row = m.page * MENU_VISIBLE + slot;
  return row < (int)m.count ? row : -1;
}

bool menuPageForward(Menu &m) {
  const uint8_t pages = (m.count + MENU_VISIBLE - 1) / MENU_VISIBLE;
  if (pages < 2) return false;
  m.page = (m.page + 1) % pages;
  return true;
}

Gesture gestureOpposite(Gesture g) {
  switch (g) {
    case G_UP: return G_DOWN;
    case G_DOWN: return G_UP;
    case G_LEFT: return G_RIGHT;
    case G_RIGHT: return G_LEFT;
    default: return G_NONE;
  }
}

void drawMenu(Arduino_GFX *g, const Menu &m, int ox, int oy) {
  g->fillRect(ox, oy, SCREEN_W, SCREEN_H, C_BG);

  g->setTextSize(2);
  g->setTextColor(C_ACCENT);
  g->setCursor(ox + 12, oy + 16);
  g->print(m.title);

  g->drawFastHLine(ox + 12, oy + 42, SCREEN_W - 24, C_DIM);

  const uint8_t first = m.page * MENU_VISIBLE;
  for (uint8_t slot = 0; slot < MENU_VISIBLE; slot++) {
    const uint8_t row = first + slot;
    if (row >= m.count) break;
    drawMenuRowAt(g, m.items[row], row == m.index, ox, oy + menuRowY(slot));
  }

  drawNavBar(g, m, ox, oy);
}

// A selection change updates two small rectangles plus the nav counter,
// instead of rebuilding and retransmitting the whole screen.
void updateMenuSelection(Menu &m, uint8_t nextIndex) {
  if (nextIndex >= m.count || nextIndex == m.index) return;
  const uint8_t oldIndex = m.index;
  m.index = nextIndex;
  markDirty();  // every selection change is worth persisting, exactly once
  const uint8_t first = m.page * MENU_VISIBLE;
  if (oldIndex >= first && oldIndex < first + MENU_VISIBLE) {
    drawMenuRowAt(panel, m.items[oldIndex], false, 0, menuRowY(oldIndex - first));
  }
  if (m.index >= first && m.index < first + MENU_VISIBLE) {
    drawMenuRowAt(panel, m.items[m.index], true, 0, menuRowY(m.index - first));
  }
  drawNavBar(panel, m, 0, 0);
}

// === Activation ===
// A port outside 1..65535 is not a port. Clamped on commit rather than
// rejected per keystroke, so you can delete back through an invalid value.
static void portCommitted() {
  long v = atol(iotPort);
  if (v < 1) v = 1;
  if (v > 65535) v = 65535;
  snprintf(iotPort, sizeof(iotPort), "%ld", v);
  refreshIotLabels();
}

// Tapping a row acts on it. For MODE the selection IS the setting, so the
// caller handles that menu itself; everything here either opens the keyboard
// on a field or runs an action.
void activateItem(int menuId, uint8_t row) {
  Menu &m = menus[menuId];
  LOGF("activate: %s / %s\n", m.title, m.items[row]);

  if (menuId == MENU_SETTINGS) {
    switch (row) {
      case SET_NAME:
        kbOpen(nametagName, sizeof(nametagName), "NAMETAG", false, nullptr);
        slideTo(SCREEN_KB, G_UP);
        return;
    }
  }

  if (menuId == MENU_IOT) {
    switch (row) {
      case IOT_BROKER:
        kbOpen(iotBroker, sizeof(iotBroker), "MQTT BROKER", false,
               refreshIotLabels);
        break;
      case IOT_PORT:
        kbOpen(iotPort, sizeof(iotPort), "MQTT PORT", true, portCommitted);
        break;
      case IOT_USER:
        kbOpen(iotUser, sizeof(iotUser), "MQTT USER", false, refreshIotLabels);
        break;
      case IOT_PASS:
        kbOpen(iotPass, sizeof(iotPass), "MQTT PASSWORD", false,
               refreshIotLabels);
        break;
      case IOT_TOPIC:
        kbOpen(iotTopic, sizeof(iotTopic), "MQTT TOPIC", false,
               refreshIotLabels);
        break;
      case IOT_SECRET:
        // Rolling the code is how a user revokes one they read out loud by
        // mistake. Any pairing part-way through is invalidated, which is the
        // whole point, so this is deliberately a single deliberate tap.
        identityRegenerateCode();
        refreshIotLabels();
        render();
        return;
      case IOT_STATUS:
        return;  // derived value; it reports, it does not act
    }
    slideTo(SCREEN_KB, G_UP);
    return;
  }

  if (menuId == MENU_SYSTEM) {
    // Any tap that is not the reset row cancels an arm in progress.
    if (row != SYS_RESET && resetArmedAt) {
      menusDisarmReset();
      refreshSysLabels();
    }
    switch (row) {
      case SYS_BRIGHT:
        // Tapping cycles up through the steps and wraps, so one row is the
        // whole control - no separate up/down affordance to fit on screen.
        brightness = (brightness + 1) % BRIGHT_COUNT;
        displayApplyBrightness();
        refreshSysLabels();
        saveSettings();
        render();
        return;
      case SYS_BATT:
        showBatteryIcon = !showBatteryIcon;
        refreshSysLabels();
        saveSettings();
        render();
        return;
      case SYS_SSID:
        kbOpen(wifiSsid, sizeof(wifiSsid), "WIFI NETWORK", false,
               refreshSysLabels);
        break;
      case SYS_PASS:
        kbOpen(wifiPass, sizeof(wifiPass), "WIFI PASSWORD", false,
               refreshSysLabels);
        break;
      case SYS_STATUS:
        return;  // read-only; it reports, it does not act
      case SYS_RESET:
        if (!resetArmed()) {
          resetArmedAt = millis();
          refreshSysLabels();
          render();
          LOGF("factory reset armed - tap again within %lums\n",
               (unsigned long)RESET_ARM_MS);
          return;
        }
        menusDisarmReset();
        factoryReset();
        // Reboot rather than carrying on. Half the firmware caches settings in
        // module statics (WiFi state, MQTT client, scene index); restarting is
        // the only way to guarantee none of them survive the wipe.
        panel->fillScreen(C_BG);
        printCentered(panel, "FACTORY RESET", 0, SCREEN_H / 2 - 20, 2, C_WARN);
        printCentered(panel, "rebooting...", 0, SCREEN_H / 2 + 10, 1, C_DIM);
        LOGF("factory reset done, rebooting\n");
        Serial.flush();
        delay(1200);
        ESP.restart();
        return;
      case SYS_CONNECT:
        if (wifiWanted) {
          wifiDisconnect();
        } else {
          wifiConnect();
        }
        refreshSysLabels();
        saveSettings();
        render();
        return;
    }
    slideTo(SCREEN_KB, G_UP);
    return;
  }

  // Everything else: the selection itself is the setting, so just persist it.
  markDirty();
}
