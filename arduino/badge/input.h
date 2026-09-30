// input.h - CST816S touch controller and gesture recognition.
//
// The only thing the rest of the firmware sees is pollGesture(), which returns
// a completed Gesture or G_NONE. Raw coordinates of the last touch are exposed
// because taps need to know where they landed.

#pragma once

#include "config.h"
#include "types.h"

// Last touch coordinates, valid when pollGesture() returns G_TAP.
extern int lastX, lastY;
// Where the current/most recent stroke began. Menus open based on which EDGE a
// stroke started at, so this matters as much as the direction.
extern int startX, startY;

void touchBegin();

// Derives a gesture from the stroke's furthest travel, firing on release.
// Using the furthest point rather than the last one means a fast flick still
// registers even if the finger drifts back before it lifts.
Gesture pollGesture();

// Periodic bus-health report, so a dead touch bus is visible in the log
// without anyone needing to touch the screen.
void touchDiag(uint32_t now);

const char *gestureWord(Gesture g);
const char *gestureArrow(Gesture g);
