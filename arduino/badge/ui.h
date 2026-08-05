// ui.h - the home screen, screen state, and gesture dispatch.
//
// This is the only module that decides what a gesture MEANS. Everything below
// it either reports raw input (input.h) or draws what it is told (display.h),
// which is what keeps the interaction rules in one readable place.

#pragma once

#include <Arduino_GFX_Library.h>

#include "config.h"
#include "types.h"

// Which screen is showing. >= 0 indexes menus[]; SCREEN_HOME and SCREEN_KB are
// the special screens.
extern int current;

// Home is a splash at boot, then a summary card, then it hands over to a mode.
extern bool splashActive;
extern bool modeActive;
extern uint32_t lastActivity;

// A full-screen message, shown until it expires. Overrides whatever the badge
// was displaying without disturbing the underlying mode, so it goes back to
// what it was doing afterwards.
void bannerShow(const char *text, uint32_t seconds);
bool bannerActive();
void drawBanner(Arduino_GFX *g, int ox, int oy);

// Full-screen padlock glyph after locking / unlocking the screen (3s).
// `unlocked` true draws the open shackle; false draws the closed lock.
void screenLockGlyphShow(bool unlocked);
bool screenLockGlyphActive();
void drawScreenLockGlyph(Arduino_GFX *g, int ox, int oy);

// Brief corner lock/unlock after pinning / unpinning a nametag background.
// Drawn bottom-right, just left of the battery gauge.
void bgPinGlyphShow(bool locked);
bool bgPinGlyphActive();
void drawBgPinGlyph(Arduino_GFX *g, int ox, int oy);

// While true, taps/swipes/holds in a running mode are ignored except the
// ~550ms long-press arm, which unlocks. Set by a 3s still-hold in a mode.
bool screenIsLocked();

void drawHome(Arduino_GFX *g, int ox, int oy);

// The hidden credits screen, reached only by holding a finger on the idle
// home card for HOLD_MS. Not on any menu, and not hinted at anywhere.
void drawCredits(Arduino_GFX *g, int ox, int oy);

void handleGesture(Gesture g);

// Boot splash expiry, idle handover, menu timeout. Called from loop().
void uiTick(uint32_t now);
