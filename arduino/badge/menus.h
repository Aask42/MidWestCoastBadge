// menus.h - the menu model, its rendering, and the navigation stack.
//
// Interaction rule, which the whole design leans on: swipes move between
// screens, taps choose items. They never overlap. Every row of every menu is on
// screen at once, so nothing scrolls and a swipe never moves the highlight.
// Each menu's nav bar names the one swipe that leaves it, and that swipe leaves
// from any row.

#pragma once

#include <Arduino_GFX_Library.h>

#include "config.h"
#include "types.h"

#define MENU_COUNT 4
#define MENU_IOT 0
#define MENU_MODE 1  // its selection IS the live mode
#define MENU_SETTINGS 2
#define MENU_SYSTEM 3

// Rows of the MQTT menu. IOT_SECRET is the pairing code a user reads off the
// badge and types into the web client; see identity.h for what it is and is
// not. The badge id used to have a row here, but the home screen already shows
// it and the broker password needs the space more.
#define IOT_ROWS 7
#define IOT_BROKER 0
#define IOT_PORT 1
#define IOT_USER 2
#define IOT_PASS 3
#define IOT_TOPIC 4
#define IOT_SECRET 5
#define IOT_STATUS 6

#define SET_ROWS 1
#define SET_NAME 0

// Factory reset lives here, under the network settings it mostly exists to
// clear. It is two-tap armed and reboots afterwards - see activateItem().
#define SYS_ROWS 7
#define SYS_BRIGHT 0
#define SYS_BATT 1
#define SYS_SSID 2
#define SYS_PASS 3
#define SYS_STATUS 4
#define SYS_CONNECT 5
#define SYS_RESET 6

extern Menu menus[];
extern const char **modeItems;

uint8_t activeMode();

// Rows that show live values are rendered into fixed buffers, so the pointers
// in menus[] stay valid for the life of the program. Call these after changing
// anything they display.
void refreshIotLabels();
void refreshModeLabels();
void refreshSetLabels();
void refreshSysLabels();

// === Rendering ===
int menuRowY(uint8_t i);  // top of SLOT i's highlight rect, screen coords

// Row index under screen slot `slot` on the menu's current page, or -1.
int menuRowAtSlot(const Menu &m, int slot);

// Advances to the next page, wrapping. No-op if the menu fits on one screen.
// Returns true if anything moved.
bool menuPageForward(Menu &m);

// The swipe that opened a menu is the opposite of the one that leaves it.
Gesture gestureOpposite(Gesture g);
void drawMenu(Arduino_GFX *g, const Menu &m, int ox, int oy);

// Repaints just the two affected rows plus the nav counter, rather than
// rebuilding and retransmitting the whole screen.
void updateMenuSelection(Menu &m, uint8_t nextIndex);

// Acts on a row. May open the keyboard, toggle a value, or navigate.
void activateItem(int menuId, uint8_t row);

// === Navigation history ===
// Screens are pushed as they are entered, so "back" returns to wherever you
// actually came from instead of assuming it was home. The deepest chain today
// is home -> menu -> keyboard, but keeping it a stack means adding a submenu
// later needs no special case.
void navPush(int screen);
int navPop();  // falls back to home if empty, so nothing can strand the user
void navClear();

// Cancels an armed factory reset. Called on any navigation, so an armed reset
// can never survive leaving the screen it was armed on.
void menusDisarmReset();
