// config.h - hardware wiring, screen geometry, palette and tuning constants.
//
// Everything in here is a compile-time constant. No state, no code, no
// dependencies on other badge headers, so any module can include it freely.
// If you are porting this firmware to a different panel or a different board,
// this file plus partitions.csv should be the only things you need to touch.

#pragma once

#include <Arduino.h>

// === Display wiring (connector 7). CS is strapped to GND on the PCB. ===
#define TFT_SCK 6
#define TFT_MOSI 7
#define TFT_DC 10
#define TFT_RST 8
#define TFT_BL 0  // LEDK, driven by PWM for brightness control

// === Touch wiring (connector 8) ===
#define TP_SDA 4
#define TP_SCL 5
#define TP_INT 3
#define TP_RST 2
#define TP_ADDR 0x15

#define SCREEN_W 240
#define SCREEN_H 320
#define SPI_SPEED 80000000

// A full-screen canvas would be 240*320*2 = 153,600 bytes, which will not
// allocate as one contiguous block on this C3. Instead the screen is composited
// one horizontal strip at a time and each strip is blitted straight to the
// panel, and still flicker-free since every pixel is written once per frame.
// Taller strips mean fewer blits AND fewer full-screen redraws per frame
// (each strip re-runs the draw calls), which is the main cost of a slide.
#define STRIP_H 80

// Set to 0 to compile out all per-gesture logging.
#define DEBUG_SERIAL 1
#if DEBUG_SERIAL
#define LOGF(...) Serial.printf(__VA_ARGS__)
#else
#define LOGF(...) \
  do {            \
  } while (0)
#endif

// Bumped by hand. Shown on the splash so a badge in someone else's hands can
// be identified without opening a serial console.
#define BADGE_VERSION "0.6.1"

// === Palette (RGB565) ===
#define C_BG 0x1082      // near-black
#define C_FG 0xFFFF      // white
#define C_DIM 0x7BEF     // grey
#define C_ACCENT 0x07FF  // cyan
#define C_NAV 0xFD20     // amber - deliberately unlike the cyan selection
#define C_NAV_FG 0x0000  // black text on the nav bar
#define C_OK 0x07E0      // green - connected / healthy
#define C_WARN 0xF800    // red - failed / disconnected

// The built-in GFX font is 5x7 on a 6x8 cell, so a glyph is 6*size wide.
#define GLYPH_W 6
#define GLYPH_H 8

// === Gesture tuning ===
// A stroke is a swipe once it travels this far; anything shorter is a tap.
// There is deliberately no gap between the two thresholds. They used to be 22
// and 12, which left a 10px band where a stroke was classified as neither and
// thrown away - and a fingertip on a 240px panel drifts well into that band
// during a perfectly ordinary tap, so taps went missing at random.
static const int SWIPE_MIN = 22;

// Polls during a stroke are milliseconds apart, so a real fingertip cannot move
// anywhere near this far between two of them. A larger jump is the controller
// glitching, and the sample is discarded rather than believed.
static const int MAX_JUMP = 60;

// A stroke starting within this many px of an edge is treated as belonging to
// that edge, whichever way it then travels.
#define EDGE_ZONE 80

// How long a finger has to sit still before the stroke becomes a hold. Long
// enough that nobody finds it by accident during an ordinary tap, which is the
// point - it is the only way to the credits screen.
static const uint32_t HOLD_MS = 3000;

#define ANIM_STEPS 3  // frames per screen transition

// === Timing ===
static const uint32_t SPLASH_MS = 3000;
// Two separate idle windows, because they answer different questions.
// MODE_IDLE_MS is how long home sits before the badge starts doing its thing on
// its own - short, because a badge on a lanyard showing a static summary card is
// the failure mode. MENU_IDLE_MS is how long an open menu survives being
// ignored, and wants to stay generous: it runs while someone is reading a list
// or deciding what to tap, and yanking that away mid-thought is worse than
// leaving it up.
static const uint32_t MODE_IDLE_MS = 5000;
static const uint32_t MENU_IDLE_MS = 15000;
static const uint32_t SLIDE_MS = 2500;         // slideshow dwell per image
static const uint32_t SAVE_DEBOUNCE_MS = 1500; // NVS write debounce
static const uint32_t WIFI_POLL_MS = 1000;

// === MQTT connect timeouts ===
// The library defaults here are 3s TCP, 120s TLS handshake and 15s for CONNACK,
// and all three block loop(). See mqttBegin() for why that adds up to the badge
// freezing rather than just being offline. Short enough that an unreachable
// broker is a hitch, long enough to cross a congested con network.
static const uint32_t MQTT_TCP_TIMEOUT_MS = 2000;
static const uint32_t MQTT_TLS_HANDSHAKE_S = 5;
static const uint32_t MQTT_SOCKET_TIMEOUT_S = 2;
static const uint32_t LENT_FRAME_MS = 40;      // ~20fps, bounded by redraw cost

// === Menu geometry ===
// Shared by the renderer and the tap hit-test so the two can never disagree
// about where a row is.
#define MENU_TOP 60
#define MENU_ROW_H 32

// How many rows fit above the nav bar. Menus longer than this paginate: the
// swipe that OPENED the menu pages forward (same direction you dragged it in),
// and the retreat swipe still leaves from anywhere. The highlight never moves
// on a swipe, so "swipes navigate, taps choose" still holds.
#define MENU_VISIBLE ((NAV_Y - MENU_TOP) / MENU_ROW_H)
#define NAV_H 36
#define NAV_Y (SCREEN_H - NAV_H)

// === Screen ids ===
// >= 0 is an index into menus[], negatives are the special screens. The
// keyboard is an id rather than an "is it up?" flag so that transitions
// involving it composite correctly - present() draws whatever id it is handed.
#define SCREEN_HOME -1
#define SCREEN_KB -2
#define SCREEN_CREDITS -3

// === Credits ===
// Reached only by holding a finger on the badge for HOLD_MS. Deliberately not
// on any menu and not hinted at on the home screen: a badge that advertises its
// own easter egg has not got one.
// The name is split across two lines rather than wrapped at draw time: it is
// the largest text on the screen and the break wants to be a decision, not
// whatever the fitter happens to land on.
#define CREDITS_NAME1 "AMELIA"
#define CREDITS_NAME2 "WIETTING"
#define CREDITS_FOR "for DEF CON 34"
#define CREDITS_AKA "AKA Aask Questions"
#define CREDITS_URL "aask.ltd"

// === Modes, matching the order of modeItems[] ===
// The MODE menu is now a flat list of "what should be on the screen": the
// nametag, the slideshow, or one specific animation held on its own. Picking a
// scene by name IS static mode - there is no separate static/scene setting to
// find, which is what made the old split confusing.
#define MODE_NAMETAG 0
#define MODE_SLIDESHOW 1
#define MODE_SCENE_FIRST 2  // rows 2..2+SHOW_COUNT-1 are individual shows

#define DEFAULT_NAME "YOUR NAME"

// === Default network ===
// Compiled in so a freshly flashed badge joins the fleet network on its own and
// is reachable by OTA without anyone first typing a passphrase on a 240px
// keyboard. These are only the starting values: anything already stored in NVS
// wins, so a badge moved to another network stays there across reflashes, and
// SETTINGS or the serial console still override them per badge.
#define DEFAULT_WIFI_SSID "wifiot"
#define DEFAULT_WIFI_PASS "WhyKnot42!"

// === Battery sense ===
// GPIO1 is the only ADC-capable pin this firmware does not already use (ADC1
// channels are GPIO0-4; 0 is the backlight, 2/3/4 are touch). It is therefore
// the most likely place a pack-sense divider would land - but that is an
// inference from the pinout, NOT something read off a schematic. If the badge
// has no divider, this pin floats and batteryValid() will report false rather
// than the firmware inventing a percentage.
//
// To confirm: measure the pack, compare against the mV in the telemetry feed.
// If they track, the divider below is right; if the reading is noise, there is
// no sense circuit and only the uptime figure is meaningful.
#define BATT_ADC_PIN 1
#define BATT_DIVIDER 2.0f  // assumes a 1:1 resistor divider

// 3x AAA alkaline: ~4.5V fresh, and the 3.3V regulator gives up somewhere
// around 3.2V. Alkaline discharge is emphatically not linear, so treat the
// percentage as a coarse gauge and the mV in the telemetry as the real data.
#define BATT_FULL_MV 4500
#define BATT_EMPTY_MV 3200

// Sampled often enough to be smooth, published rarely enough not to flood.
static const uint32_t BATT_SAMPLE_MS = 2000;
static const uint32_t TELEMETRY_MS = 30000;
