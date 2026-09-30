// modes.h - the full-screen displays the badge falls into when left idle.
//
// All four are procedural. That was originally to keep flash free for image
// assets; it still means the badge has something to show before any assets
// exist. Real slideshow images belong here as const arrays - the app partition
// has ~2.9MB spare for exactly that.

#pragma once

#include <Arduino_GFX_Library.h>

#include "config.h"
#include "types.h"

#define SCENE_COUNT 8

// Lenticular bitmaps live on the `storage` LittleFS partition, NOT in the
// firmware. Keeping them out of the app means an OTA never re-sends artwork
// that has not changed, and the same bitmap is not duplicated across both A/B
// slots. Drop a new .raw in and it appears in the SHOW menu with no rebuild.
//
// They are pre-woven by tools/interlace.py: even columns are the left-eye
// view, odd the right. Never scale one or offset it by an odd number of
// pixels - that swaps the eyes and inverts the depth.
#define IMAGE_MAX 12
extern uint8_t imageCount;
const char *imageName(uint8_t i);

// Mounts storage and scans it. Safe if the partition is empty or unformatted;
// imageCount is simply 0 and only the procedural scenes are offered.
void imagesBegin();

// Total pickable things: procedural scenes first, then bitmaps.
#define SHOW_COUNT (SCENE_COUNT + imageCount)
const char *showName(uint8_t i);

// Name of scene i, for the SETTINGS picker. Wraps.
const char *sceneName(uint8_t i);

void drawModeScreen(Arduino_GFX *g, int ox, int oy);

// True once SNTP has given us a wall clock, which is what lets badges agree on
// slideshow phase without talking to each other.
bool timeIsSynced();

// Slideshow advance and lenticular rotation. Returns true if the frame changed
// and the screen needs repainting.
bool modesTick(uint32_t now);

// Called when a mode starts, to reset per-mode animation state.
void modesEnter(uint32_t now);
void modesExit();

// Lets interactive modes consume gestures before the UI treats them as an
// exit. The WiFi scanner uses vertical swipes to browse its result list.
bool modesHandleGesture(Gesture gesture);

// Long-press on unlocked nametag: pin the current background under the name.
// Persists.
bool modesPinCurrentAsNametag();

// Long-press on a locked nametag: resume rotating procedural backgrounds.
bool modesUnpinNametag();
