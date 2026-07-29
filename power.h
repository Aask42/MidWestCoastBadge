// power.h - battery sense and runtime telemetry.
//
// Two separate things, and only one of them is guaranteed to work:
//
//   Uptime      Always correct. This is the measurement that actually answers
//               "how long does a set of AAAs last": the badge reports how long
//               it has been alive, and the LAST report before it dies is the
//               answer. No calibration required.
//
//   Battery mV  Requires the pack to be wired to an ADC pin through a divider.
//               BATT_ADC_PIN is a best guess (see config.h) - if this badge has
//               no sense circuit the reading is a floating pin and meaningless.
//               batteryValid() says which case you are in, and the icon and the
//               telemetry both report "unknown" rather than inventing a number.
//
// Telemetry goes out on <topic>/badge/<id>/telemetry, NOT retained: these are
// samples over time, and a retained one would just be a stale reading forever.

#pragma once

#include <Arduino_GFX_Library.h>

void powerBegin();
void powerTick(uint32_t now);

// Millivolts at the pack, corrected for the divider. 0 if unavailable.
uint16_t batteryMillivolts();

// 0..100, or -1 when there is no usable reading.
int batteryPercent();

// False when the ADC reading is outside anything a real pack could produce,
// which is what a disconnected sense pin looks like.
bool batteryValid();

uint32_t uptimeSeconds();

// Draws the corner badge. Caller decides whether it is wanted; see
// showBatteryIcon in store.h.
void drawBatteryIcon(Arduino_GFX *g, int ox, int oy);
