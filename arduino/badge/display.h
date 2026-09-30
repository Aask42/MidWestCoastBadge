// display.h - the panel, the strip compositor, and shared drawing primitives.
//
// Every draw function takes an (ox, oy) offset so that two screens can be
// composited side by side during a transition; Arduino_GFX clips anything that
// lands off-canvas. Draw functions must therefore never assume they are drawing
// at 0,0 and never read the offset back from global state.

#pragma once

#include <Arduino_GFX_Library.h>

#include "config.h"
#include "types.h"

extern Arduino_GFX *panel;
extern Arduino_Canvas *strip;
extern bool stripReady;  // false = no strip buffer, animation is skipped

// A gesture that completed while a frame was being composited. present() polls
// touch between strips so a tap cannot be swallowed by a redraw, but it cannot
// act on one from inside the compositor - loop() drains this instead.
extern Gesture pendingGesture;

// Brings up the panel and tries to allocate the strip buffer. Returns false if
// the panel itself failed; a missing strip buffer is survivable and is reported
// through stripReady rather than as a failure.
bool displayBegin();

// Backlight. Level is an index into BRIGHT_PCT[] and lives in store.h, because
// it is persisted; this just pushes the current value at the hardware.
void displayApplyBrightness();

// Toggle 180° display rotation and save the setting.
void displayFlip();

// === Primitives ===
int textWidth(const char *s, uint8_t size);
void printCentered(Arduino_GFX *g, const char *s, int ox, int y, uint8_t size,
                   uint16_t color);

// One glyph per line, so an edge label can run down the side of the screen.
// `cy` is the vertical centre of the resulting column.
void printStacked(Arduino_GFX *g, const char *s, int x, int cy, uint16_t color);

// A bordered, centre-labelled key. Used by the keyboard for every key it draws.
void drawKey(Arduino_GFX *g, int x, int y, int w, int h, const char *label,
             uint16_t bg, uint16_t fg, uint8_t size);

// === Compositing ===
// Draws screen `id` at the given offset. The id fully determines what is drawn
// - no ambient state - so a transition can composite any two screens.
void drawScreen(Arduino_GFX *g, int id, int ox, int oy);

// Composites up to two screens at the given offsets and pushes the result to
// the panel one strip at a time. Passing outId == inId just draws one screen.
void present(int outId, int outDx, int outDy, int inId, int inDx, int inDy);

// Repaints the current screen in place.
void render();

// Brief white flash — confirms deliberate mode actions (nametag pin / unpin).
void flashConfirm();

// Slides from the current screen to `next` in the direction of the gesture,
// then makes `next` current. The incoming screen enters from the edge the
// swipe came from.
void slideTo(int next, Gesture dir);
