// keyboard.h - the on-screen keyboard.
//
// One keyboard serves every text field in the firmware. Adding another editable
// field is a single kbOpen() call at the point of use, not another copy of this.
//
// The keyboard is a screen id (SCREEN_KB) rather than an "is it up?" flag, so
// it slides in and out through the same compositor as everything else.
//
// Gestures while it is up: swipe LEFT deletes a character (the direction the
// caret moves), swipe DOWN cancels (pushing it back to the edge it rose from).
// Cancel used to be any swipe, which cannot coexist with swipe-to-delete.

#pragma once

#include <Arduino_GFX_Library.h>

#include "config.h"
#include "types.h"

// Sized for the longest field the keyboard has to serve, which is a 63-char
// WPA2 passphrase. Shorter fields are capped by their own buffer, not by this.
#define EDIT_MAX 64

// Opens the keyboard on `target`. `size` is the destination buffer's size, so
// the entry length is limited to what will actually fit when committed - what
// you can type is exactly what will be stored. `onCommit` may be null.
void kbOpen(char *target, size_t size, const char *title, bool numeric,
            void (*onCommit)());

void drawKeyboard(Arduino_GFX *g, int ox, int oy);

// Repaints only the entry band, straight to the panel. A keystroke changes a
// few hundred pixels; routing it through render() rebuilt all 240x320 through
// the strip buffer (~150KB of SPI, ~36ms) and made typing feel sluggish.
void redrawKbEntry();

KbResult handleKeyboardTap(int x, int y);

// Swipe-to-delete. Returns false if the caret is already at the start.
bool kbDeleteAtCaret();

const char *kbTitle();
