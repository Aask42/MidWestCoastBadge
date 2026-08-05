// store.h - every value that survives a reboot, and the NVS plumbing.
//
// Two write policies, deliberately different:
//
//   markDirty()    Debounced by SAVE_DEBOUNCE_MS. For incidental changes like
//                  a menu cursor moving. NVS lives in flash, and scrolling a
//                  list should not burn a write cycle per row.
//   saveSettings() Immediate. For deliberate one-shot commits - a name, a
//                  chosen mode, a brightness step. Losing one of those to a
//                  battery pull inside the debounce window is not a trade
//                  worth making on a badge.
//
// The keypair and pairing code are NOT here; they live in their own NVS
// namespace via identity.h, so a settings wipe cannot destroy a badge's
// cryptographic identity.

#pragma once

#include <Arduino.h>

#include "config.h"

// === Nametag ===
extern char nametagName[24];

// Name colour mode. 0 = rainbow (animated per letter); the rest are solids.
// Tap cycles in SETTINGS — same pattern as brightness.
#define NAME_COLOR_RAINBOW 0
#define NAME_COLOR_COUNT 7
extern uint8_t nametagColor;
extern const char *const NAME_COLOR_NAMES[NAME_COLOR_COUNT];
extern const uint16_t NAME_COLOR_RGB[NAME_COLOR_COUNT];

// When true, nametag keeps nameBgShow as its background instead of rotating
// procedural scenes. Set by long-pressing the unlocked nametag rotator.
extern bool nameBgLocked;
extern uint8_t nameBgShow;

// === MQTT / IoT config ===
extern char iotBroker[32];
extern char iotPort[6];
extern char iotUser[16];
extern char iotPass[48];
extern char iotTopic[24];
extern char iotClientId[16];
extern bool iotOnline;

// === Display brightness ===
// Discrete steps, because a percentage you tap through is easier to hit than a
// slider on a 240px panel and there is no useful resolution between them.
extern const uint8_t BRIGHT_PCT[];
#define BRIGHT_COUNT 5
#define BL_FREQ 5000  // well above flicker, well below LEDC's ceiling at 8 bit
#define BL_RES 8
extern uint8_t brightness;

// Corner battery gauge. On by default because the first thing anyone does with
// a new pack is watch it drain.
extern bool showBatteryIcon;

// WiFi observations stay on the badge unless the owner opts into publishing
// them to the configured MQTT fleet topic.
extern bool shareWifiScans;

// Remote control gate. On by default so a fresh badge accepts OTA / rename /
// setShow from the fleet. Toggle via SETTINGS → fleet pushes. When off,
// authenticated banners still land (fleet and owner); rename / setShow / OTA
// are dropped for both — USB flash is then the only firmware update path.
// When on, fleet can do all four; owner can still rename / setShow / banner
// but never OTA.
extern bool acceptFleetPushes;

// User-defined popup shown on demand from the settings menu.
extern char popupText[48];

// 180-degree display rotation for badges worn upside-down.
extern bool displayFlipped;

// === WiFi credentials ===
extern char wifiSsid[33];  // 32 chars max per 802.11
extern char wifiPass[64];  // 63 chars max for WPA2
// Radio enable. Default false — credentials can be stored without joining.
// SYSTEM → WiFi radio toggles this and persists it.
extern bool wifiWanted;

// Copies into a fixed buffer and always terminates. strncpy alone does not
// terminate when the source fills the buffer, which is exactly the case a
// 32-char broker name hits.
void setField(char *dst, size_t n, const char *src);

void regenerateClientId();

void loadSettings();
void saveSettings();
void markDirty();

// Called from loop(); flushes a debounced write once things have settled.
void storeTick(uint32_t now);

// Wipes settings back to compiled-in defaults. Does NOT touch identity.
void factoryReset();
