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

void drawHome(Arduino_GFX *g, int ox, int oy);

// The hidden credits screen, reached only by holding a finger on home or on a
// running mode for HOLD_MS. Not on any menu, and not hinted at anywhere.
void drawCredits(Arduino_GFX *g, int ox, int oy);

void handleGesture(Gesture g);

// Boot splash expiry, idle handover, menu timeout. Called from loop().
void uiTick(uint32_t now);
