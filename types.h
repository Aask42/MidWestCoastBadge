// types.h - shared enums and the menu model.
//
// Kept separate from config.h so that modules which only need a Gesture do not
// drag in every pin definition, and separate from any module header so there is
// no include cycle between (say) input and ui.

#pragma once

#include <Arduino.h>

// Direction of a completed stroke. G_TAP is a stroke that never travelled far
// enough to be a swipe; G_NONE means no gesture completed this poll.
enum Gesture { G_NONE, G_UP, G_DOWN, G_LEFT, G_RIGHT, G_TAP };

// Result of one touch poll. T_FAILED must be distinguished from T_UP: treating
// a bad I2C read as "finger lifted" chops a swipe in half and loses it.
enum TouchState { T_FAILED, T_UP, T_DOWN };

// Outcome of a tap landing on the keyboard.
//   KB_IGNORED - nothing under the finger.
//   KB_ENTRY   - only the entry band changed, so just that strip is repainted.
//   KB_REDRAW  - the key faces changed too (case or page), so the whole screen
//                is rebuilt.
//   KB_DONE    - OK was pressed and the value is committed; close the keyboard.
enum KbResult { KB_IGNORED, KB_ENTRY, KB_REDRAW, KB_DONE };

// Case is a three-state cycle on one key rather than a shift plus a separate
// caps lock: KC_SHIFT applies to exactly the next character and then falls back
// to KC_LOWER, KC_CAPS sticks. Cycling avoids double-tap timing, which is
// unreliable on this panel and invisible to the user.
enum KbCase { KC_LOWER, KC_SHIFT, KC_CAPS };

// Each menu is anchored to a screen edge and is dragged inward from it, the way
// a phone's notification shade works. `retreat` pushes it back out to that
// edge, and is the only swipe a menu responds to.
struct Menu {
  const char *title;
  const char **items;
  uint8_t count;
  Gesture retreat;  // swipe that leaves the menu; named on its nav bar
  uint8_t index;
  uint8_t page;     // first visible row is page * MENU_VISIBLE
};
