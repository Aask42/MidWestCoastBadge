// purpose: DEF CON 34 badge - ST7789 display + CST816S touch, swipe menus
// usage: arduino-cli compile -u -p <port>
//          --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc
//          --build-property upload.maximum_size=4063232 badge
//
//        CDCOnBoot=cdc puts the serial log on the USB port; it also builds
//        without it, the log just moves to the UART pins.
//
//        The sketch-local partitions.csv overrides the board's PartitionScheme
//        entirely (see that file for the layout and why), giving a 3.875MB app
//        partition instead of the stock 1.25MB. That matters because linking
//        WiFi took the build from ~400KB to ~1.05MB, which is 80% of the stock
//        partition and leaves nothing for image assets.
//
//        upload.maximum_size is a SEPARATE board property from the partition
//        table - it is only the ceiling arduino-cli size-checks against. Left
//        alone it still reads 1.25MB and will fail the build once assets push
//        past that, even though the real partition has room. Keep the two in
//        step: this number is app0's size from partitions.csv.
//
// Screen flow:
//   boot -> DEF CON 34 splash -> home (shows the active mode)
//   home idle for IDLE_MS -> the selected mode runs full screen
//   tap while a mode is running   -> back to home
//   swipe while a mode is running -> back to home AND the swipe still opens
//                                    the menu it was aimed at, so waking the
//                                    badge never costs an extra gesture
//
// Navigation from home works by EDGE, like a phone's notification shade:
// swiping at an edge opens that edge's menu whichever way the stroke travels,
// because at the bottom edge a finger can only move up. Direction is only the
// fallback for strokes that start mid-screen.
//
//   top edge    -> Mode menu,         drag it back up   to leave
//   bottom edge -> IoT Config menu,   drag it back down to leave
//   left edge   -> Settings menu,     drag back left    to leave
//   right edge  -> right placeholder, drag back right   to leave
//
// Inside a menu the two input types do not overlap: swipes move between
// screens, taps choose items. Every row is on screen at once, so there is
// nothing to scroll and a swipe never moves the highlight. Each menu carries a
// bottom nav bar naming the one swipe that leaves it, and that swipe leaves
// from any row.
//
// Tapping a row acts on it immediately - action rows (the nametag, and the
// MQTT broker/port/user/topic) open the keyboard on the first tap. The MODE
// menu is the one exception, because its rows are a choice rather than an
// action: the first tap moves the highlight so a mis-tap can be corrected,
// and tapping the highlighted row again confirms the mode and returns home.
//
// The keyboard is a screen in its own right, so it slides in and out like
// every other screen. OK commits; swiping LEFT deletes a character, the same
// direction the caret moves; swiping DOWN pushes it back to the edge it rose
// from and cancels. Screens are pushed onto a small nav stack as they are
// entered, so "back" always lands on whatever opened the current screen.
//
// Menu selections and the IoT config persist in NVS on a 1.5s debounce, so
// scrolling a list does not burn a flash write per row. Deliberate one-shot
// commits - a name, a chosen mode - are flushed immediately instead, because
// losing one to a battery pull in that window is not an acceptable trade.
//
// Screen changes are animated as a real slide: the outgoing and incoming
// screens are composited at an offset into a reusable strip buffer, which is
// blitted to the panel a band at a time.

#include <Arduino_GFX_Library.h>
#include <Preferences.h>
#include <WiFi.h>
#include <Wire.h>

// === Display wiring (connector 7). CS is strapped to GND on the PCB. ===
#define TFT_SCK 6
#define TFT_MOSI 7
#define TFT_DC 10
#define TFT_RST 8
#define TFT_BL 0  // LEDK, high = on

// === Touch wiring (connector 8) ===
#define TP_SDA 4
#define TP_SCL 5
#define TP_INT 3
#define TP_RST 2
#define TP_ADDR 0x15

#define SCREEN_W 240
#define SCREEN_H 320

Arduino_DataBus *bus = new Arduino_ESP32SPI(
    TFT_DC, GFX_NOT_DEFINED /* CS tied to GND */, TFT_SCK, TFT_MOSI,
    GFX_NOT_DEFINED /* MISO */);

Arduino_GFX *panel = new Arduino_ST7789(
    bus, TFT_RST, 0 /* rotation */, true /* IPS */, SCREEN_W, SCREEN_H);

// A full-screen canvas would be 240*320*2 = 153,600 bytes, which will not
// allocate as one contiguous block on this C3. Instead the screen is composited
// one horizontal strip at a time and each strip is blitted straight to the
// panel, and still flicker-free since every pixel is written once per frame.
// Taller strips mean fewer blits AND fewer full-screen redraws per frame
// (each strip re-runs the draw calls), which is the main cost of a slide.
#define STRIP_H 80
#define SPI_SPEED 80000000
Arduino_Canvas *strip = new Arduino_Canvas(SCREEN_W, STRIP_H, panel);
bool stripReady = false;

// Set to 0 to compile out all per-gesture logging.
#define DEBUG_SERIAL 1
#if DEBUG_SERIAL
#define LOGF(...) Serial.printf(__VA_ARGS__)
#else
#define LOGF(...) \
  do {            \
  } while (0)
#endif

// === Palette (RGB565) ===
#define C_BG 0x1082      // near-black
#define C_FG 0xFFFF      // white
#define C_DIM 0x7BEF     // grey
#define C_ACCENT 0x07FF  // cyan
#define C_NAV 0xFD20     // amber - deliberately unlike the cyan selection
#define C_NAV_FG 0x0000  // black text on the nav bar

// === Gestures ===
// (Declared up here because the Arduino preprocessor emits function
// prototypes at the top of the file, ahead of any later declarations.)
enum Gesture { G_NONE, G_UP, G_DOWN, G_LEFT, G_RIGHT, G_TAP };

// Result of one touch poll. FAILED must be distinguished from UP: treating a
// bad read as "finger lifted" chops a swipe in half and the gesture is lost.
enum TouchState { T_FAILED, T_UP, T_DOWN };

// Outcome of a tap landing on the keyboard. KB_IGNORED: nothing under the
// finger. KB_ENTRY: only the entry band changed, so just that strip is
// repainted. KB_REDRAW: the key faces changed too (case or page), so the whole
// screen is rebuilt. KB_DONE: OK was pressed and the value is committed, so
// the caller closes the keyboard.
enum KbResult { KB_IGNORED, KB_ENTRY, KB_REDRAW, KB_DONE };

// Case is a three-state cycle on one key rather than a shift plus a separate
// caps lock: KC_SHIFT applies to exactly the next character and then falls
// back to KC_LOWER, KC_CAPS sticks. Cycling avoids double-tap timing, which
// is unreliable on a resistive-feeling panel and invisible to the user.
enum KbCase { KC_LOWER, KC_SHIFT, KC_CAPS };

// A stroke is a swipe once it travels this far; anything shorter is a tap.
// These are deliberately the SAME threshold. They used to be 22 and 12, which
// left a 10px band where a stroke was classified as neither and thrown away -
// and a fingertip on a 240px panel drifts well into that band during a
// perfectly ordinary tap, so taps went missing at random.
static const int SWIPE_MIN = 22;
static const int ANIM_STEPS = 3;  // frames per screen transition

// Polls during a stroke are milliseconds apart, so a real fingertip cannot
// move anywhere near this far between two of them. A larger jump is the
// controller glitching, and the sample is discarded rather than believed.
static const int MAX_JUMP = 60;

// Boot splash duration, then how long home sits idle before the badge drops
// into whatever mode is selected, and the slideshow's per-image dwell.
static const uint32_t SPLASH_MS = 3000;
static const uint32_t IDLE_MS = 15000;
static const uint32_t SLIDE_MS = 2500;

// Modes, matching the order of modeItems[].
#define MODE_SLIDESHOW 0
#define MODE_NAMETAG 1
#define MODE_STATIC 2
#define MODE_LENTICULAR 3

#define DEFAULT_NAME "YOUR NAME"
char nametagName[24] = DEFAULT_NAME;

// === IoT config ===
// Editable from the IoT menu and persisted; the MQTT client itself is not
// wired up yet, so `iotOnline` stays false and the status row reads offline.
char iotBroker[32] = "mqtt.local";
char iotPort[6] = "1883";
char iotUser[16] = "badge";
char iotTopic[24] = "dc34";
char iotClientId[16] = "";
bool iotOnline = false;

// === Display brightness ===
// LEDK is driven by PWM rather than a plain high, so the level is adjustable.
// Discrete steps, because a percentage you tap through is easier to hit than a
// slider on a 240px panel and there is no useful resolution between them.
static const uint8_t BRIGHT_PCT[] = {15, 30, 50, 75, 100};
#define BRIGHT_COUNT 5
#define BL_FREQ 5000  // well above flicker, well below LEDC's ceiling at 8 bit
#define BL_RES 8
uint8_t brightness = BRIGHT_COUNT - 1;  // default full
bool blPwm = false;  // false = LEDC unavailable, backlight pinned on

// === WiFi ===
// SSID and passphrase are stored here and the connection is attempted on
// demand (and at boot if an SSID is already saved). Nothing blocks on it:
// WiFi.begin is asynchronous and the status row polls.
char wifiSsid[33] = "";   // 32 chars max per 802.11
char wifiPass[64] = "";   // 63 chars max for WPA2
bool wifiWanted = false;  // true once a connection has been asked for

// Menu row geometry - shared by the renderer and the tap hit-test so the two
// can never disagree about where a row is.
#define MENU_TOP 60
#define MENU_ROW_H 32

// Bottom nav bar
#define NAV_H 36
#define NAV_Y (SCREEN_H - NAV_H)

// The built-in GFX font is 5x7 on a 6x8 cell, so a glyph is 6*size wide.
#define GLYPH_W 6
#define GLYPH_H 8

// === Menu model ===
struct Menu {
  const char *title;
  const char **items;
  uint8_t count;
  Gesture retreat;  // swipe that leaves the menu; named on its nav bar
  uint8_t index;
};

// The IoT rows show live values, so their text is rebuilt into these buffers
// whenever a field changes. iotItems[] points at the buffers, never at the
// values themselves, so the pointers stay valid for the life of the program.
#define IOT_ROWS 6
char iotLabel[IOT_ROWS][40];
const char *iotItems[IOT_ROWS] = {iotLabel[0], iotLabel[1], iotLabel[2],
                                  iotLabel[3], iotLabel[4], iotLabel[5]};

static const char *modeItems[] = {"slideshow", "nametag", "static image",
                                  "lenticular cube"};
static const char *settingsItems[] = {"set name", "clear name"};
// The SYSTEM menu shows live values too, so it uses the same rendered-label
// approach as the IoT menu: the pointers are fixed, the text behind them is
// rebuilt whenever something changes.
#define SYS_ROWS 5
char sysLabel[SYS_ROWS][40];
const char *sysItems[SYS_ROWS] = {sysLabel[0], sysLabel[1], sysLabel[2],
                                  sysLabel[3], sysLabel[4]};

// Each menu is anchored to a screen edge and is dragged inward from it, the
// way a phone's notification shade works. `retreat` pushes it back out to that
// edge, and is the only swipe a menu responds to.
Menu menus[] = {
    {"IoT CONFIG", iotItems, IOT_ROWS, G_DOWN, 0},  // bottom edge, push down
    {"MODE", modeItems, 4, G_UP, 0},                // top edge, push back up
    {"SETTINGS", settingsItems, 2, G_LEFT, 0},      // left edge, push back left
    {"SYSTEM", sysItems, SYS_ROWS, G_RIGHT, 0},     // right edge, push right
};

#define MENU_COUNT 4
#define MENU_IOT 0
#define MENU_MODE 1  // index of the MODE menu; its selection is the live mode
#define MENU_SETTINGS 2
#define MENU_SYSTEM 3

// Rows of the SYSTEM menu.
#define SYS_BRIGHT 0
#define SYS_SSID 1
#define SYS_PASS 2
#define SYS_STATUS 3
#define SYS_CONNECT 4

// Rows of the IoT menu, matching iotLabel[] / iotItems[].
#define IOT_BROKER 0
#define IOT_PORT 1
#define IOT_USER 2
#define IOT_TOPIC 3
#define IOT_STATUS 4
#define IOT_ID 5

// Rows of the SETTINGS menu.
#define SET_NAME 0
#define SET_CLEAR 1

// A stroke starting within this many px of an edge is treated as belonging to
// that edge, whichever way it then travels.
#define EDGE_ZONE 80

// Screen ids: >= 0 is an index into menus[], negatives are the special screens.
// The keyboard is an id rather than an "is it up?" flag so that transitions
// involving it composite correctly - present() draws whatever id it is handed.
#define SCREEN_HOME -1
#define SCREEN_KB -2

int current = SCREEN_HOME;

// === Navigation history ===
// Screens are pushed as they are entered, so "back" returns to wherever you
// actually came from instead of assuming it was home. Today the deepest chain
// is home -> menu -> keyboard, but keeping it a stack means adding a submenu
// later does not need a special case, and the keyboard no longer needs its own
// private return-to variable.
#define NAV_MAX 4
int navStack[NAV_MAX];
uint8_t navDepth = 0;

void navPush(int screen) {
  if (navDepth < NAV_MAX) navStack[navDepth++] = screen;
}

// Where "back" leads. Falls back to home if the history is somehow empty, so
// a lost push can never strand the user on a screen with no way out.
int navPop() { return navDepth ? navStack[--navDepth] : SCREEN_HOME; }

void navClear() { navDepth = 0; }

// Home is a splash at boot, then a summary card, then hands over to the mode.
bool splashActive = true;
bool modeActive = false;
uint32_t splashStart = 0;
uint32_t lastActivity = 0;
uint32_t lastSlide = 0;
uint8_t slideIndex = 0;
uint32_t lastFrame = 0;  // last lenticular animation frame

// WiFi status is polled rather than driven by events, because the only thing
// that needs to react is a label. Once a second is far more often than a join
// changes state, and costs nothing.
uint32_t lastWifiPoll = 0;
wl_status_t lastWifiStatus = WL_NO_SHIELD;
static const uint32_t WIFI_POLL_MS = 1000;

// Frame interval for the lenticular cube. A full redraw through the strip
// buffer is the floor here (~36ms measured), so this throttles rather than
// paces: it keeps the loop polling touch between frames instead of spinning.
static const uint32_t LENT_FRAME_MS = 40;

// === Persistence ===
// Menu selections and the nametag survive a reboot. Writes are debounced
// because NVS lives in flash and scrolling a menu should not burn a write
// cycle per row.
Preferences prefs;
bool settingsDirty = false;
uint32_t lastChange = 0;
static const uint32_t SAVE_DEBOUNCE_MS = 1500;

// === On-screen keyboard ===
// One keyboard serves every text row. `editTarget` is the buffer the commit
// writes back into, so adding another editable field is a one-line change at
// the call site rather than another copy of the keyboard.
char *editTarget = nullptr;
size_t editTargetSize = 0;
const char *editTitle = "";
bool editNumeric = false;
void (*editOnCommit)() = nullptr;  // optional post-commit hook, may be null

// Sized for the longest field the keyboard has to serve, which is a 63-char
// WPA2 passphrase. Shorter fields are capped by their own buffer, not by this.
#define EDIT_MAX 64
char editBuf[EDIT_MAX];
uint8_t editLen = 0;
uint8_t editPos = 0;  // insertion caret, 0..editLen
KbCase kbCase = KC_LOWER;
bool kbSymbols = false;

// Two pages. The letter page keeps . - _ inline because a hostname needs them
// constantly and a page flip per dot would be miserable; the symbol page then
// covers all 32 ASCII punctuation marks, in ASCII order, 8 per row. Between
// the two, every printable ASCII character is reachable.
static const char *KB_ROWS_ALPHA[] = {"1234567890", "qwertyuiop", "asdfghjkl",
                                      "zxcvbnm.-_"};
static const char *KB_ROWS_SYM[] = {"!\"#$%&'(", ")*+,-./:", ";<=>?@[\\",
                                    "]^_`{|}~"};
#define KB_ROW_COUNT 4

const char *kbRow(int r) {
  return kbSymbols ? KB_ROWS_SYM[r] : KB_ROWS_ALPHA[r];
}

// Vertical bands: title, entry field, caret/CLR row, key rows, action row,
// hint. Every one of these is derived, never repeated, so the renderer and the
// hit-test cannot drift apart.
#define KB_TITLE_Y 20
#define KB_FIELD_Y 34
#define KB_FIELD_H 40
#define KB_HINT_Y 78
#define KB_EDIT_Y 92
#define KB_EDIT_H 32
#define KB_TOP 128
#define KB_KEY_H 36

// The action row is flush with the bottom edge of the panel, and taller than
// a character key. It used to stop 36px short of the bottom: the touch log
// showed every missed tap landing in that strip, all of them aimed at DEL or
// OK, because a finger reaching for the bottom corner of a screen goes to the
// actual bottom, not to wherever the row happens to end.
#define KB_ACTION_H 48
#define KB_ACTION_Y (SCREEN_H - KB_ACTION_H)

// A numeric field (the port) shows only the digit row. The shorter keyboard is
// pushed down against the action row rather than left floating mid-screen, so
// DEL and OK stay exactly where they are on the full keyboard.
int kbRowCount() { return editNumeric ? 1 : KB_ROW_COUNT; }
int kbTop() { return KB_TOP + (KB_ROW_COUNT - kbRowCount()) * KB_KEY_H; }

// Caret row: LEFT | RIGHT | CLR, three equal keys.
#define KB_EDIT_KEY_W (SCREEN_W / 3)

// Action row: DEL | case | page | SPACE | OK, widths summing to SCREEN_W.
#define KB_DEL_W 44
#define KB_CASE_W 40
#define KB_PAGE_W 40
#define KB_OK_W 44
#define KB_SPACE_W (SCREEN_W - KB_DEL_W - KB_CASE_W - KB_PAGE_W - KB_OK_W)

// === Touch ===
bool touchDown = false;
int startX = 0, startY = 0, lastX = 0, lastY = 0;
int bestDx = 0, bestDy = 0;  // furthest travel seen during the stroke

void touchReset() {
  pinMode(TP_RST, OUTPUT);
  digitalWrite(TP_RST, LOW);
  delay(20);
  digitalWrite(TP_RST, HIGH);
  delay(60);
}

TouchState touchRead(int &x, int &y) {
  Wire.beginTransmission(TP_ADDR);
  Wire.write(0x01);  // GestureID, FingerNum, XposH/L, YposH/L
  if (Wire.endTransmission(false) != 0) return T_FAILED;
  if (Wire.requestFrom((uint8_t)TP_ADDR, (uint8_t)6) != 6) return T_FAILED;

  uint8_t b[6];
  for (int i = 0; i < 6; i++) b[i] = Wire.read();

  // An idle/failed read comes back as all 0xFF, which decodes to "1 finger at
  // 4095,4095" and fires phantom taps.
  uint8_t fingers = b[1];
  if (fingers == 0xFF) return T_FAILED;
  if (fingers == 0) return T_UP;
  if (fingers > 2) return T_FAILED;

  x = ((b[2] & 0x0F) << 8) | b[3];
  y = ((b[4] & 0x0F) << 8) | b[5];
  if (x >= SCREEN_W || y >= SCREEN_H) return T_FAILED;

  return T_DOWN;
}

// Derives a gesture from the stroke's furthest travel. Fires on release.
// Using the furthest point rather than the last one means a fast flick still
// registers even if the finger drifts back before it lifts.
Gesture pollGesture() {
  int x, y;
  TouchState st = touchRead(x, y);

  if (st == T_FAILED) return G_NONE;  // ignore the sample, keep the stroke

  if (st == T_DOWN) {
    if (!touchDown) {
      touchDown = true;
      startX = x;
      startY = y;
      bestDx = 0;
      bestDy = 0;
    } else if (abs(x - lastX) > MAX_JUMP || abs(y - lastY) > MAX_JUMP) {
      // The controller occasionally emits a wild coordinate. Because travel is
      // tracked as the furthest point ever seen, a single bad sample turns a
      // short stroke into a full swipe - the log caught a 46px stroke
      // reporting 173px of travel and firing a phantom LEFT/RIGHT.
      LOGF("touch glitch %d,%d after %d,%d - sample dropped\n", x, y, lastX,
           lastY);
      return G_NONE;
    }
    lastX = x;
    lastY = y;
    int dx = x - startX;
    int dy = y - startY;
    if (abs(dx) > abs(bestDx)) bestDx = dx;
    if (abs(dy) > abs(bestDy)) bestDy = dy;
    return G_NONE;
  }

  if (!touchDown) return G_NONE;
  touchDown = false;

  const int dx = bestDx;
  const int dy = bestDy;
  const int adx = abs(dx), ady = abs(dy);

  // Every completed stroke now resolves to something: it is a swipe if it
  // travelled far enough on its dominant axis, and a tap otherwise. No stroke
  // is ever silently discarded.
  Gesture g;
  if (adx >= SWIPE_MIN || ady >= SWIPE_MIN) {
    if (adx > ady) {
      g = dx > 0 ? G_RIGHT : G_LEFT;
    } else {
      g = dy > 0 ? G_DOWN : G_UP;
    }
  } else {
    g = G_TAP;
  }

  LOGF("stroke start=%d,%d end=%d,%d best=%d,%d -> %s\n", startX, startY, lastX,
       lastY, bestDx, bestDy, g == G_TAP ? "TAP" : gestureWord(g));
  return g;
}

// === Text helpers ===
const char *gestureWord(Gesture g) {
  switch (g) {
    case G_UP: return "UP";
    case G_DOWN: return "DOWN";
    case G_LEFT: return "LEFT";
    case G_RIGHT: return "RIGHT";
    default: return "?";
  }
}

const char *gestureArrow(Gesture g) {
  switch (g) {
    case G_UP: return "\x18";  // GFX code page arrows
    case G_DOWN: return "\x19";
    case G_LEFT: return "\x1B";
    case G_RIGHT: return "\x1A";
    default: return "";
  }
}

int textWidth(const char *s, uint8_t size) {
  return (int)strlen(s) * GLYPH_W * size;
}

// Draws `s` horizontally centred on the screen at row `y`.
void printCentered(Arduino_GFX *g, const char *s, int ox, int y, uint8_t size,
                   uint16_t color) {
  g->setTextSize(size);
  g->setTextColor(color);
  g->setCursor(ox + (SCREEN_W - textWidth(s, size)) / 2, y);
  g->print(s);
}

uint8_t activeMode() { return menus[MENU_MODE].index; }

// Copies into a fixed buffer and always terminates. strncpy alone does not
// terminate when the source fills the buffer, which is exactly the case a
// 32-char broker name hits.
void setField(char *dst, size_t n, const char *src) {
  strncpy(dst, src, n - 1);
  dst[n - 1] = '\0';
}

// A client id that is stable per badge but distinct between badges, so two
// badges on the same broker do not fight over one MQTT session.
void regenerateClientId() {
  snprintf(iotClientId, sizeof(iotClientId), "dc34-%04lx",
           (unsigned long)(esp_random() & 0xFFFF));
}

// === Brightness ===
// Falls back to a plain digital high if LEDC will not attach, so a failure
// here dims nothing rather than leaving the user with a black screen.
void applyBrightness() {
  if (brightness >= BRIGHT_COUNT) brightness = BRIGHT_COUNT - 1;
  if (blPwm) {
    ledcWrite(TFT_BL, (BRIGHT_PCT[brightness] * 255) / 100);
  } else {
    digitalWrite(TFT_BL, HIGH);
  }
}

// === WiFi ===
const char *wifiStateText() {
  switch (WiFi.status()) {
    case WL_CONNECTED: return "connected";
    case WL_NO_SSID_AVAIL: return "no such network";
    case WL_CONNECT_FAILED: return "auth failed";
    case WL_CONNECTION_LOST: return "lost";
    case WL_DISCONNECTED: return "disconnected";
    case WL_IDLE_STATUS: return "idle";
    default: return wifiWanted ? "connecting" : "off";
  }
}

// Asynchronous: this returns immediately and the status row tracks progress.
// Blocking on a join would freeze the UI for seconds on a bad password.
void wifiConnect() {
  if (wifiSsid[0] == '\0') {
    LOGF("wifi: no SSID set\n");
    return;
  }
  wifiWanted = true;
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid, wifiPass);
  LOGF("wifi: connecting to '%s'\n", wifiSsid);
}

void wifiDisconnect() {
  wifiWanted = false;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  LOGF("wifi: off\n");
}

// Rebuilds the SYSTEM menu text from the live values.
void refreshSysLabels() {
  snprintf(sysLabel[SYS_BRIGHT], sizeof(sysLabel[0]), "brightness: %u%%",
           (unsigned)BRIGHT_PCT[brightness]);
  snprintf(sysLabel[SYS_SSID], sizeof(sysLabel[0]), "wifi: %s",
           wifiSsid[0] ? wifiSsid : "--");
  // Masked in the list so a shoulder-surfer at a con does not read it off the
  // badge. It is shown in clear while editing, where you need to check it.
  snprintf(sysLabel[SYS_PASS], sizeof(sysLabel[0]), "pw: %s",
           wifiPass[0] ? "********" : "--");
  if (WiFi.status() == WL_CONNECTED) {
    snprintf(sysLabel[SYS_STATUS], sizeof(sysLabel[0]), "ip: %s",
             WiFi.localIP().toString().c_str());
  } else {
    snprintf(sysLabel[SYS_STATUS], sizeof(sysLabel[0]), "status: %s",
             wifiStateText());
  }
  snprintf(sysLabel[SYS_CONNECT], sizeof(sysLabel[0]), "%s",
           wifiWanted ? "disconnect" : "connect");
}

// Rebuilds the IoT menu text from the live values. Called after every change.
void refreshIotLabels() {
  snprintf(iotLabel[IOT_BROKER], sizeof(iotLabel[0]), "broker: %s",
           iotBroker[0] ? iotBroker : "--");
  snprintf(iotLabel[IOT_PORT], sizeof(iotLabel[0]), "port: %s", iotPort);
  snprintf(iotLabel[IOT_USER], sizeof(iotLabel[0]), "user: %s",
           iotUser[0] ? iotUser : "--");
  snprintf(iotLabel[IOT_TOPIC], sizeof(iotLabel[0]), "topic: %s",
           iotTopic[0] ? iotTopic : "--");
  snprintf(iotLabel[IOT_STATUS], sizeof(iotLabel[0]), "status: %s",
           iotOnline ? "online" : "offline");
  snprintf(iotLabel[IOT_ID], sizeof(iotLabel[0]), "id: %s", iotClientId);
}

// === Persistence ===
void loadSettings() {
  prefs.begin("badge", true);  // read-only
  for (int i = 0; i < MENU_COUNT; i++) {
    char key[8];
    snprintf(key, sizeof(key), "m%d", i);
    uint8_t v = prefs.getUChar(key, 0);
    if (v < menus[i].count) menus[i].index = v;
  }
  prefs.getString("name", nametagName, sizeof(nametagName));
  prefs.getString("broker", iotBroker, sizeof(iotBroker));
  prefs.getString("port", iotPort, sizeof(iotPort));
  prefs.getString("user", iotUser, sizeof(iotUser));
  prefs.getString("topic", iotTopic, sizeof(iotTopic));
  prefs.getString("cid", iotClientId, sizeof(iotClientId));
  prefs.getString("ssid", wifiSsid, sizeof(wifiSsid));
  prefs.getString("wpw", wifiPass, sizeof(wifiPass));
  brightness = prefs.getUChar("bright", BRIGHT_COUNT - 1);
  if (brightness >= BRIGHT_COUNT) brightness = BRIGHT_COUNT - 1;
  prefs.end();

  // First boot on a blank NVS, or a partition wiped by a reflash.
  if (iotClientId[0] == '\0') {
    regenerateClientId();
    markDirty();
  }
  refreshIotLabels();
  refreshSysLabels();
  LOGF("settings loaded: mode=%s name=%s broker=%s:%s id=%s ssid=%s bright=%u%%\n",
       modeItems[activeMode()], nametagName, iotBroker, iotPort, iotClientId,
       wifiSsid[0] ? wifiSsid : "--", (unsigned)BRIGHT_PCT[brightness]);
}

void saveSettings() {
  prefs.begin("badge", false);
  for (int i = 0; i < MENU_COUNT; i++) {
    char key[8];
    snprintf(key, sizeof(key), "m%d", i);
    prefs.putUChar(key, menus[i].index);
  }
  prefs.putString("name", nametagName);
  prefs.putString("broker", iotBroker);
  prefs.putString("port", iotPort);
  prefs.putString("user", iotUser);
  prefs.putString("topic", iotTopic);
  prefs.putString("cid", iotClientId);
  prefs.putString("ssid", wifiSsid);
  prefs.putString("wpw", wifiPass);
  prefs.putUChar("bright", brightness);
  prefs.end();
  settingsDirty = false;
  LOGF("settings saved\n");
}

// Mark state as needing a flush; loop() writes it once things settle.
void markDirty() {
  settingsDirty = true;
  lastChange = millis();
}

// === Mode rendering ===
// Stand-ins until real assets exist. Everything is procedural so it costs no
// flash. That mattered more before WiFi: linking it took the build from ~400KB
// to ~1.05MB, which is 80% of the default 1.2MB app partition. Build with
// PartitionScheme=huge_app (3MB app) before adding real image assets.

void drawTestPattern(Arduino_GFX *g, int ox, int oy, uint8_t which) {
  switch (which % 3) {
    case 0: {  // SMPTE-ish colour bars
      static const uint16_t bars[8] = {0xFFFF, 0xFFE0, 0x07FF, 0x07E0,
                                       0xF81F, 0xF800, 0x001F, 0x0000};
      const int w = SCREEN_W / 8;
      for (int i = 0; i < 8; i++) {
        g->fillRect(ox + i * w, oy, w, SCREEN_H, bars[i]);
      }
      break;
    }
    case 1: {  // concentric frames - good for checking edges and centring
      g->fillRect(ox, oy, SCREEN_W, SCREEN_H, C_BG);
      static const uint16_t cols[4] = {C_ACCENT, 0xF81F, 0xFFE0, C_FG};
      for (int i = 0; i < 10; i++) {
        g->drawRect(ox + i * 10, oy + i * 13, SCREEN_W - i * 20,
                    SCREEN_H - i * 26, cols[i % 4]);
      }
      break;
    }
    default: {  // horizontal bands
      for (int y = 0; y < SCREEN_H; y += 16) {
        g->fillRect(ox, oy + y, SCREEN_W, 16,
                    ((y / 16) % 2) ? C_ACCENT : C_BG);
      }
      break;
    }
  }
}

void drawNametag(Arduino_GFX *g, int ox, int oy) {
  g->fillRect(ox, oy, SCREEN_W, SCREEN_H, C_ACCENT);
  g->fillRect(ox + 8, oy + 8, SCREEN_W - 16, SCREEN_H - 16, C_BG);

  printCentered(g, "DEF CON 34", ox, oy + 90, 1, C_DIM);

  // Shrink the name until it fits the panel width.
  uint8_t size = 4;
  while (size > 1 && textWidth(nametagName, size) > SCREEN_W - 32) size--;
  printCentered(g, nametagName, ox, oy + 140, size, C_ACCENT);

  printCentered(g, "touch to exit", ox, oy + SCREEN_H - 40, 1, C_DIM);
}

// === Lenticular 3D cube ===
// The lens over the panel sends even columns to one eye and odd columns to the
// other. Rendering a proper stereo pair into those two column sets gives real
// binocular depth - the cube sits in front of and behind the glass - rather
// than a painted-on illusion. Both eyes see the same wireframe from slightly
// different viewpoints; the only thing separating them is column parity.

// Unit cube centred on the origin, so its half-size is 1 world unit.
static const float CUBE_VERTS[8][3] = {
    {-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
    {-1, -1, 1},  {1, -1, 1},  {1, 1, 1},  {-1, 1, 1},
};
static const uint8_t CUBE_EDGES[12][2] = {
    {0, 1}, {1, 2}, {2, 3}, {3, 0},  // back face
    {4, 5}, {5, 6}, {6, 7}, {7, 4},  // front face
    {0, 4}, {1, 5}, {2, 6}, {3, 7},  // struts between them
};

// Tuning. FOCAL is pixels per world unit at unit depth; DIST puts the cube far
// enough back that no vertex reaches the camera. At these values the cube
// sweeps x 48..192 and y 77..223 over a full rotation, so it never clips.
//
// EYE_SEP is THE knob for how strong the 3D reads, and the one to tune against
// the real lens. Disparity is EYE_SEP * FOCAL * (1/z - 1/DIST), which here
// spans about -3px (behind the glass) to +6px (in front of it). Much past that
// and the two images stop fusing and read as ghosting instead of depth.
static const float CUBE_FOCAL = 200.0f;
static const float CUBE_DIST = 5.0f;
static const float EYE_SEP = 0.28f;
#define LENT_PERIOD_MS 6000  // one full rotation

// Left and right eye colours. Through a correctly aligned lens each eye sees a
// single solid colour, which makes this double as an alignment check: if the
// pitch is off you see magenta blend or shimmer instead. Set both to the same
// colour for a straight lenticular render with no rivalry.
#define C_EYE_L 0x07FF  // cyan
#define C_EYE_R 0xFD20  // amber

// Projects the cube for one eye. Convergence is set at CUBE_DIST, so geometry
// at that depth lands on the glass with zero parallax and everything else
// splits either side of it.
void projectCube(float angle, float eyeX, int cx, int cy, int out[8][2]) {
  const float sy = sinf(angle), cy_ = cosf(angle);
  const float sp = sinf(angle * 0.6f), cp = cosf(angle * 0.6f);
  const float fConv = CUBE_FOCAL / CUBE_DIST;

  for (int i = 0; i < 8; i++) {
    const float vx = CUBE_VERTS[i][0], vy = CUBE_VERTS[i][1],
                vz = CUBE_VERTS[i][2];
    // Yaw about Y, then pitch about X, at a slower rate so the tumble does not
    // repeat every revolution.
    const float x1 = vx * cy_ + vz * sy;
    const float z1 = -vx * sy + vz * cy_;
    const float y2 = vy * cp - z1 * sp;
    const float z2 = vy * sp + z1 * cp;

    float z = z2 + CUBE_DIST;
    if (z < 0.5f) z = 0.5f;  // never divide through a vertex behind the camera
    const float f = CUBE_FOCAL / z;

    int px = cx + (int)((x1 - eyeX) * f + eyeX * fConv);
    int py = cy + (int)(y2 * f);
    // Bresenham walks every pixel between the endpoints, so a wild coordinate
    // would cost real time. The projection cannot produce one, but clamp
    // anyway rather than trust that.
    out[i][0] = px < -SCREEN_W ? -SCREEN_W : (px > 2 * SCREEN_W ? 2 * SCREEN_W : px);
    out[i][1] = py < -SCREEN_H ? -SCREEN_H : (py > 2 * SCREEN_H ? 2 * SCREEN_H : py);
  }
}

// Bresenham that plots only columns of the given parity, so the two eye images
// interleave on the panel without ever touching each other's pixels.
void drawLineParity(Arduino_GFX *g, int x0, int y0, int x1, int y1,
                    uint16_t color, int parity) {
  // This runs once per strip, and a given edge is visible in only one or two
  // of the four. GFX clips per pixel, so without this the walk still steps
  // through every pixel of every edge four times over just to throw the
  // results away - the clip is correct but it is not free.
  const int h = g->height();
  if ((y0 < 0 && y1 < 0) || (y0 >= h && y1 >= h)) return;

  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (;;) {
    if ((x0 & 1) == parity) g->drawPixel(x0, y0, color);
    if (x0 == x1 && y0 == y1) break;
    const int e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y0 += sy;
    }
  }
}

// Held in a global and advanced once per frame in loop(). It must NOT be
// derived from millis() in here: this is called once per strip, and millis()
// moves between strips, which would shear the cube across the four bands.
float lentAngle = 0.0f;

void drawLenticular(Arduino_GFX *g, int ox, int oy) {
  g->fillRect(ox, oy, SCREEN_W, SCREEN_H, C_BG);

  const int cx = ox + SCREEN_W / 2;
  const int cy = oy + SCREEN_H / 2 - 10;

  int L[8][2], R[8][2];
  projectCube(lentAngle, -EYE_SEP / 2, cx, cy, L);
  projectCube(lentAngle, +EYE_SEP / 2, cx, cy, R);

  for (int e = 0; e < 12; e++) {
    const uint8_t a = CUBE_EDGES[e][0], b = CUBE_EDGES[e][1];
    drawLineParity(g, L[a][0], L[a][1], L[b][0], L[b][1], C_EYE_L, 0);
    drawLineParity(g, R[a][0], R[a][1], R[b][0], R[b][1], C_EYE_R, 1);
  }

  // Pitch-alignment target, kept from the old test pattern: a 1px interlace of
  // the same two colours the cube uses.
  const int stripY = oy + SCREEN_H - 36;
  for (int x = 0; x < SCREEN_W; x++) {
    g->drawFastVLine(ox + x, stripY, 14, (x & 1) ? C_EYE_R : C_EYE_L);
  }

  g->fillRect(ox, oy + SCREEN_H - 20, SCREEN_W, 20, C_BG);
  printCentered(g, "lenticular cube", ox, oy + SCREEN_H - 14, 1, C_DIM);
}

void drawModeScreen(Arduino_GFX *g, int ox, int oy) {
  switch (activeMode()) {
    case MODE_NAMETAG:
      drawNametag(g, ox, oy);
      return;
    case MODE_LENTICULAR:
      drawLenticular(g, ox, oy);
      return;
    case MODE_STATIC:
      drawTestPattern(g, ox, oy, 1);
      break;
    case MODE_SLIDESHOW:
    default:
      drawTestPattern(g, ox, oy, slideIndex);
      break;
  }
  // Caption strip so the mode is identifiable over the test pattern.
  g->fillRect(ox, oy + SCREEN_H - 20, SCREEN_W, 20, C_BG);
  printCentered(g, modeItems[activeMode()], ox, oy + SCREEN_H - 14, 1, C_DIM);
}

// === On-screen keyboard ===
// Ten keys per row at 24px is about as small as a fingertip can reliably hit
// on a 240px panel, so the layout is four rows and a shift key rather than
// separate letter/symbol pages.

// Geometry of one key in row `r`, so drawing and hit-testing agree. Rows are
// 10 keys on the letter page and 8 on the symbol page, so the width is derived
// per row rather than fixed.
void kbKeyRect(int r, int idx, int &x, int &w) {
  const int n = strlen(kbRow(r));
  w = SCREEN_W / n;
  x = (SCREEN_W - w * n) / 2 + idx * w;
}

// The character row `r` key `idx` produces, honouring case. Digits and
// punctuation have no upper case, so toupper leaves them alone.
char kbCharAt(int r, int idx) {
  char c = kbRow(r)[idx];
  return (kbCase != KC_LOWER) ? toupper(c) : c;
}

void drawKey(Arduino_GFX *g, int x, int y, int w, int h, const char *label,
             uint16_t bg, uint16_t fg, uint8_t size) {
  g->fillRect(x + 1, y + 1, w - 2, h - 2, bg);
  g->drawRect(x + 1, y + 1, w - 2, h - 2, C_DIM);
  g->setTextSize(size);
  g->setTextColor(fg);
  g->setCursor(x + (w - textWidth(label, size)) / 2,
               y + (h - GLYPH_H * size) / 2);
  g->print(label);
}

// Draws the entry field: text scrolled so the caret is always visible, with a
// vertical caret bar drawn at the insertion point rather than an appended
// underscore, so the caret can sit in the middle of the text.
void drawEntryField(Arduino_GFX *g, int ox, int oy) {
  const int fx = ox + 14;
  const int fy = oy + KB_FIELD_Y;
  const int fw = SCREEN_W - 28;

  g->drawRect(ox + 8, fy, SCREEN_W - 16, KB_FIELD_H, C_ACCENT);

  // Shrink until the whole value fits; past that the window scrolls instead.
  uint8_t size = 3;
  while (size > 1 && textWidth(editBuf, size) + GLYPH_W * size > fw) size--;
  const int cellW = GLYPH_W * size;
  const int maxChars = fw / cellW;

  // Keep the caret inside the window: scroll only when it would fall off the
  // right edge, so typing at the end tracks but browsing left does not jump.
  int first = 0;
  if (editPos > maxChars - 1) first = editPos - (maxChars - 1);

  char window[EDIT_MAX + 1];
  snprintf(window, sizeof(window), "%.*s", maxChars, editBuf + first);

  const int ty = fy + (KB_FIELD_H - GLYPH_H * size) / 2;
  g->setTextSize(size);
  g->setTextColor(C_ACCENT);
  g->setCursor(fx, ty);
  g->print(window);

  g->fillRect(fx + (editPos - first) * cellW, ty - 2, 2, GLYPH_H * size + 4,
              C_FG);
}

// Everything above the key grid: title, entry field, hint line and caret row.
// This is exactly the band a keystroke changes, so it is split out to be
// repainted on its own - see redrawKbEntry().
#define KB_ENTRY_TOP KB_TITLE_Y
#define KB_ENTRY_BOTTOM (KB_EDIT_Y + KB_EDIT_H)

void drawKbEntry(Arduino_GFX *g, int ox, int oy) {
  printCentered(g, editTitle, ox, oy + KB_TITLE_Y, 1, C_DIM);
  drawEntryField(g, ox, oy);

  char room[16];
  snprintf(room, sizeof(room), "%u left",
           (unsigned)(editTargetSize - 1 - editLen));
  g->setTextSize(1);
  g->setTextColor(C_DIM);
  g->setCursor(ox + SCREEN_W - textWidth(room, 1) - 10, oy + KB_HINT_Y);
  g->print(room);
  // Name both swipe bindings. "Any swipe cancels" is no longer true, and an
  // undiscoverable delete gesture is no better than not having one.
  char hint[24];
  snprintf(hint, sizeof(hint), "%s del  %s back", gestureArrow(G_LEFT),
           gestureArrow(G_DOWN));
  g->setCursor(ox + 10, oy + KB_HINT_Y);
  g->print(hint);

  // Caret row. The arrows move the insertion point; CLR empties the field so
  // replacing a 30-character broker name is one tap, not thirty.
  const int ey = oy + KB_EDIT_Y;
  drawKey(g, ox, ey, KB_EDIT_KEY_W, KB_EDIT_H, gestureArrow(G_LEFT), C_BG,
          editPos > 0 ? C_FG : C_DIM, 2);
  drawKey(g, ox + KB_EDIT_KEY_W, ey, KB_EDIT_KEY_W, KB_EDIT_H,
          gestureArrow(G_RIGHT), C_BG, editPos < editLen ? C_FG : C_DIM, 2);
  drawKey(g, ox + 2 * KB_EDIT_KEY_W, ey, SCREEN_W - 2 * KB_EDIT_KEY_W,
          KB_EDIT_H, "CLR", C_BG, editLen ? C_NAV : C_DIM, 1);
}

// Repaints only the entry band, straight to the panel. A keystroke changes a
// few hundred pixels; routing it through render() redrew all 240x320 through
// the strip buffer - ~150KB of SPI and about 36ms per character, which is what
// made typing feel sluggish.
void redrawKbEntry() {
  panel->fillRect(0, KB_ENTRY_TOP, SCREEN_W, KB_ENTRY_BOTTOM - KB_ENTRY_TOP,
                  C_BG);
  drawKbEntry(panel, 0, 0);
}

void drawKeyboard(Arduino_GFX *g, int ox, int oy) {
  g->fillRect(ox, oy, SCREEN_W, SCREEN_H, C_BG);
  drawKbEntry(g, ox, oy);

  const int rows = kbRowCount();
  for (int r = 0; r < rows; r++) {
    const int n = strlen(kbRow(r));
    for (int i = 0; i < n; i++) {
      int x, w;
      kbKeyRect(r, i, x, w);
      char label[2] = {kbCharAt(r, i), '\0'};
      drawKey(g, ox + x, oy + kbTop() + r * KB_KEY_H, w, KB_KEY_H, label, C_BG,
              C_FG, 2);
    }
  }

  // Action row: DEL | case | page | SPACE | OK. The case key shows the case
  // you are about to type, so it reads as state rather than as a command.
  const int ay = oy + KB_ACTION_Y;
  int x = ox;
  drawKey(g, x, ay, KB_DEL_W, KB_ACTION_H, "DEL", C_BG, C_NAV, 1);
  x += KB_DEL_W;
  if (editNumeric) {
    // A port has no case and no spaces: one inert filler rather than three
    // keys that would do nothing if tapped.
    drawKey(g, x, ay, KB_CASE_W + KB_PAGE_W + KB_SPACE_W, KB_ACTION_H, "", C_BG,
            C_DIM, 1);
  } else {
    const char *caseLabel =
        kbCase == KC_CAPS ? "ABC" : (kbCase == KC_SHIFT ? "Abc" : "abc");
    const bool caseOn = kbCase != KC_LOWER;
    drawKey(g, x, ay, KB_CASE_W, KB_ACTION_H, kbSymbols ? "" : caseLabel,
            caseOn && !kbSymbols ? C_ACCENT : C_BG,
            caseOn && !kbSymbols ? C_BG : C_FG, 1);
    x += KB_CASE_W;
    drawKey(g, x, ay, KB_PAGE_W, KB_ACTION_H, kbSymbols ? "a-z" : "!#/",
            kbSymbols ? C_ACCENT : C_BG, kbSymbols ? C_BG : C_FG, 1);
    x += KB_PAGE_W;
    drawKey(g, x, ay, KB_SPACE_W, KB_ACTION_H, "SPACE", C_BG, C_FG, 1);
  }
  drawKey(g, ox + SCREEN_W - KB_OK_W, ay, KB_OK_W, KB_ACTION_H, "OK", C_NAV,
          C_NAV_FG, 1);
}

// Inserts at the caret rather than at the end, so a typo in the middle of a
// long broker name is a fix rather than a retype. The buffer is capped by the
// destination field, not by EDIT_MAX, so what you can type is exactly what
// will be stored.
void kbInsert(char c) {
  if (editLen + 1 >= (uint8_t)editTargetSize) return;
  for (int i = editLen; i > editPos; i--) editBuf[i] = editBuf[i - 1];
  editBuf[editPos] = c;
  editLen++;
  editPos++;
  editBuf[editLen] = '\0';
}

// Deletes the character to the left of the caret, like every other keyboard.
void kbBackspace() {
  if (editPos == 0) return;
  for (int i = editPos - 1; i < editLen - 1; i++) editBuf[i] = editBuf[i + 1];
  editLen--;
  editPos--;
  editBuf[editLen] = '\0';
}

void kbClear() {
  editLen = 0;
  editPos = 0;
  editBuf[0] = '\0';
}

// Opens the keyboard on `target`. `size` is the destination buffer's size, so
// the entry length is limited to what will actually fit when it is committed.
void kbOpen(char *target, size_t size, const char *title, bool numeric,
            void (*onCommit)()) {
  editTarget = target;
  editTargetSize = size < EDIT_MAX ? size : EDIT_MAX;
  editTitle = title;
  editNumeric = numeric;
  editOnCommit = onCommit;
  kbCase = KC_LOWER;
  kbSymbols = false;  // a numeric field has no symbol page to fall back to
  setField(editBuf, editTargetSize, target);
  editLen = strlen(editBuf);
  editPos = editLen;  // caret at the end, ready to keep typing
  navPush(current);   // so closing the keyboard returns to this menu
  LOGF("edit '%s' start, value '%s'\n", title, editBuf);
}

void kbCommit() {
  // An empty value would render as a blank nametag or an unusable broker;
  // keep the previous one instead of storing nothing.
  if (editLen > 0 && editTarget) {
    setField(editTarget, editTargetSize, editBuf);
    if (editOnCommit) editOnCommit();
    // Written now, not on the 1.5s debounce. The debounce exists so that
    // scrolling a menu does not burn a flash write per row; committing a name
    // is one deliberate act, and losing it to a battery pull between OK and
    // the flush is exactly the failure a badge cannot afford.
    saveSettings();
    LOGF("edit '%s' commit -> '%s' (saved)\n", editTitle, editTarget);
  } else {
    LOGF("edit '%s' committed empty, keeping previous\n", editTitle);
  }
}

// A port outside 1..65535 is not a port. Clamped on commit rather than
// rejected per keystroke, so you can delete back through an invalid value.
void portCommitted() {
  long v = atol(iotPort);
  if (v < 1) v = 1;
  if (v > 65535) v = 65535;
  snprintf(iotPort, sizeof(iotPort), "%ld", v);
  refreshIotLabels();
}

KbResult handleKeyboardTap(int x, int y) {
  // Caret row.
  if (y >= KB_EDIT_Y && y < KB_EDIT_Y + KB_EDIT_H) {
    if (x < KB_EDIT_KEY_W) {
      if (editPos == 0) return KB_IGNORED;
      editPos--;
    } else if (x < 2 * KB_EDIT_KEY_W) {
      if (editPos >= editLen) return KB_IGNORED;
      editPos++;
    } else {
      if (editLen == 0) return KB_IGNORED;
      kbClear();
    }
    return KB_ENTRY;
  }

  // Character rows.
  const int rows = kbRowCount();
  for (int r = 0; r < rows; r++) {
    const int ry = kbTop() + r * KB_KEY_H;
    if (y >= ry && y < ry + KB_KEY_H) {
      const int n = strlen(kbRow(r));
      for (int i = 0; i < n; i++) {
        int kx, kw;
        kbKeyRect(r, i, kx, kw);
        if (x >= kx && x < kx + kw) {
          kbInsert(kbCharAt(r, i));
          // One-shot shift falls back to lower case after the character it
          // was meant for; caps lock stays until it is cycled off. Dropping
          // out of shift re-letters every key, so that case - and only that
          // case - needs the full redraw.
          if (kbCase == KC_SHIFT) {
            kbCase = KC_LOWER;
            return KB_REDRAW;
          }
          return KB_ENTRY;
        }
      }
      return KB_IGNORED;
    }
  }

  // Action row, which runs to the bottom edge of the panel.
  if (y >= KB_ACTION_Y) {
    if (x < KB_DEL_W) {
      if (editPos == 0) return KB_IGNORED;
      kbBackspace();
      return KB_ENTRY;
    }
    if (x >= SCREEN_W - KB_OK_W) {
      kbCommit();
      return KB_DONE;
    }
    if (editNumeric) return KB_IGNORED;  // inert filler where the rest would be
    if (x < KB_DEL_W + KB_CASE_W) {
      if (kbSymbols) return KB_IGNORED;  // punctuation has no case
      kbCase = (KbCase)((kbCase + 1) % 3);
      return KB_REDRAW;  // every letter key changes case
    }
    if (x < KB_DEL_W + KB_CASE_W + KB_PAGE_W) {
      kbSymbols = !kbSymbols;
      return KB_REDRAW;  // the whole grid is replaced
    }
    kbInsert(' ');
    return KB_ENTRY;
  }
  return KB_IGNORED;
}

// === Drawing ===
// Every draw takes an (ox, oy) offset so two screens can be composited
// side by side during a transition. GFX clips anything off-canvas.

// One glyph per line, so an edge label can run down the side of the screen.
// The built-in font cannot rotate, and a stacked word is legible enough for a
// hint. `cy` is the vertical centre of the resulting column.
void printStacked(Arduino_GFX *g, const char *s, int x, int cy,
                  uint16_t color) {
  const int n = strlen(s);
  g->setTextSize(1);
  g->setTextColor(color);
  for (int i = 0; i < n; i++) {
    g->setCursor(x, cy - (n * GLYPH_H) / 2 + i * GLYPH_H);
    g->write(s[i]);
  }
}

// The four swipe affordances drawn around the edge of the home screen.
void drawHomeHints(Arduino_GFX *g, int ox, int oy) {
  // Each hint sits on the edge you swipe from, and its arrow points the way
  // that edge's menu is dragged in: down from the top, up from the bottom.
  char buf[24];

  snprintf(buf, sizeof(buf), "Mode %s", gestureArrow(G_DOWN));
  printCentered(g, buf, ox, oy + 10, 1, C_FG);

  snprintf(buf, sizeof(buf), "IoT Config %s", gestureArrow(G_UP));
  printCentered(g, buf, ox, oy + SCREEN_H - 18, 1, C_FG);

  // Side hints name their menu now that both edges lead somewhere real.
  const int cy = oy + SCREEN_H / 2;
  g->setTextSize(1);
  g->setTextColor(C_ACCENT);
  g->setCursor(ox + 4, cy - 60);
  g->print(gestureArrow(G_RIGHT));  // left edge drags rightward
  printStacked(g, "SETTINGS", ox + 4, cy, C_DIM);

  g->setTextColor(C_ACCENT);
  g->setCursor(ox + SCREEN_W - 10, cy - 60);
  g->print(gestureArrow(G_LEFT));  // right edge drags leftward
  printStacked(g, "SYSTEM", ox + SCREEN_W - 10, cy, C_DIM);
}

void drawHome(Arduino_GFX *g, int ox, int oy) {
  g->fillRect(ox, oy, SCREEN_W, SCREEN_H, C_BG);

  if (splashActive) {
    printCentered(g, "DEF CON 34", ox, oy + 130, 3, C_ACCENT);
    printCentered(g, "swipe to navigate", ox, oy + 170, 1, C_DIM);
    return;  // splash stays clean: no edge hints competing with the title
  }

  printCentered(g, "DEF CON 34", ox, oy + 110, 2, C_ACCENT);
  printCentered(g, "MODE", ox, oy + 148, 1, C_DIM);
  printCentered(g, modeItems[activeMode()], ox, oy + 166, 2, C_FG);

  drawHomeHints(g, ox, oy);
}

// The nav bar names the exact swipe that leaves this menu, so the way out is
// never a guess.
void drawNavBar(Arduino_GFX *g, const Menu &m, int ox, int oy) {
  g->fillRect(ox, oy + NAV_Y, SCREEN_W, NAV_H, C_NAV);

  char label[24];
  snprintf(label, sizeof(label), "%s SWIPE %s", gestureArrow(m.retreat),
           gestureWord(m.retreat));

  g->setTextSize(2);
  g->setTextColor(C_NAV_FG);
  g->setCursor(ox + 8, oy + NAV_Y + (NAV_H - GLYPH_H * 2) / 2);
  g->print(label);

  char count[12];
  snprintf(count, sizeof(count), "%u/%u", (unsigned)m.index + 1,
           (unsigned)m.count);
  g->setTextSize(1);
  g->setCursor(ox + SCREEN_W - textWidth(count, 1) - 8,
               oy + NAV_Y + (NAV_H - GLYPH_H) / 2);
  g->print(count);
}

// One menu row, highlight included. Shared by the full redraw and the
// single-row update so the two can never disagree about geometry.
// `y` is the top of the row's highlight rectangle.
void drawMenuRowAt(Arduino_GFX *g, const char *label, bool selected, int ox,
                   int y) {
  g->fillRect(ox + 6, y, SCREEN_W - 12, MENU_ROW_H - 4,
              selected ? C_ACCENT : C_BG);

  // IoT rows carry a value, so they can outrun the row at size 2; drop a size
  // rather than clip mid-word. A full-length broker name overruns even at
  // size 1, so past that point it is truncated with a marker - GFX would
  // otherwise run the text off the panel with no sign anything was lost.
  const int roomPx = SCREEN_W - 28;
  uint8_t size = 2;
  while (size > 1 && textWidth(label, size) > roomPx) size--;

  char fitted[48];
  const int maxChars = roomPx / (GLYPH_W * size);
  if ((int)strlen(label) > maxChars) {
    snprintf(fitted, sizeof(fitted), "%.*s~", maxChars - 1, label);
    label = fitted;
  }

  g->setTextSize(size);
  g->setTextColor(selected ? C_BG : C_FG);
  g->setCursor(ox + 14, y + (MENU_ROW_H - 4 - GLYPH_H * size) / 2);
  g->print(label);
}

// Top of row `i`'s highlight rectangle, in screen coordinates.
int menuRowY(uint8_t i) { return MENU_TOP + i * MENU_ROW_H; }

void drawMenu(Arduino_GFX *g, const Menu &m, int ox, int oy) {
  g->fillRect(ox, oy, SCREEN_W, SCREEN_H, C_BG);

  g->setTextSize(2);
  g->setTextColor(C_ACCENT);
  g->setCursor(ox + 12, oy + 16);
  g->print(m.title);

  g->drawFastHLine(ox + 12, oy + 42, SCREEN_W - 24, C_DIM);

  for (uint8_t i = 0; i < m.count; i++) {
    drawMenuRowAt(g, m.items[i], i == m.index, ox, oy + menuRowY(i));
  }

  drawNavBar(g, m, ox, oy);
}

// Draws screen `id` at the given offset. The id fully determines what is
// drawn - no ambient state - so a transition can composite any two screens.
void drawScreen(Arduino_GFX *g, int id, int ox, int oy) {
  if (id == SCREEN_KB) {
    drawKeyboard(g, ox, oy);
  } else if (id == SCREEN_HOME) {
    if (modeActive) {
      drawModeScreen(g, ox, oy);
    } else {
      drawHome(g, ox, oy);
    }
  } else {
    drawMenu(g, menus[id], ox, oy);
  }
}

// Composites up to two screens at the given offsets and pushes the result to
// the panel one strip at a time. Passing outId == inId just draws one screen.
void present(int outId, int outDx, int outDy, int inId, int inDx, int inDy) {
  if (!stripReady) {
    // No strip buffer - fall back to drawing straight to the panel.
    drawScreen(panel, inId, inDx, inDy);
    return;
  }
  uint16_t *fb = strip->getFramebuffer();
  for (int y = 0; y < SCREEN_H; y += STRIP_H) {
    strip->fillScreen(C_BG);
    // Shift by -y so this strip shows the right slice of each screen.
    if (outId != inId || outDx != inDx || outDy != inDy) {
      drawScreen(strip, outId, outDx, outDy - y);
    }
    drawScreen(strip, inId, inDx, inDy - y);
    panel->draw16bitRGBBitmap(0, y, fb, SCREEN_W, STRIP_H);
    yield();  // a full redraw is ~1.5MB of SPI; keep the task WDT fed
  }
}

void render() { present(current, 0, 0, current, 0, 0); }

// A selection change updates two small rectangles plus the nav counter,
// instead of rebuilding and retransmitting the whole screen.
void updateMenuSelection(Menu &m, uint8_t nextIndex) {
  if (nextIndex >= m.count || nextIndex == m.index) return;
  const uint8_t oldIndex = m.index;
  m.index = nextIndex;
  markDirty();  // every selection change is worth persisting, exactly once
  drawMenuRowAt(panel, m.items[oldIndex], false, 0, menuRowY(oldIndex));
  drawMenuRowAt(panel, m.items[m.index], true, 0, menuRowY(m.index));
  drawNavBar(panel, m, 0, 0);
}

// Slides from the current screen to `next` in the direction of the gesture.
// The incoming screen enters from the edge the swipe came from.
void slideTo(int next, Gesture dir) {
  // Without the strip buffer there is nothing to composite into, and stepping
  // straight on the panel would just flash partial frames. Cut to the target.
  if (!stripReady) {
    current = next;
    render();
    return;
  }

  for (int s = 1; s <= ANIM_STEPS; s++) {
    // Ease-out so the motion decelerates into place.
    int32_t t = (int32_t)s * 100 / ANIM_STEPS;
    int32_t eased = 100 - ((100 - t) * (100 - t)) / 100;

    int outDx = 0, outDy = 0, inDx = 0, inDy = 0;
    switch (dir) {
      case G_DOWN:  // new screen drops in from the top
        outDy = (SCREEN_H * eased) / 100;
        inDy = outDy - SCREEN_H;
        break;
      case G_UP:  // new screen rises in from the bottom
        outDy = -(SCREEN_H * eased) / 100;
        inDy = outDy + SCREEN_H;
        break;
      case G_LEFT:  // new screen comes in from the right
        outDx = -(SCREEN_W * eased) / 100;
        inDx = outDx + SCREEN_W;
        break;
      case G_RIGHT:  // new screen comes in from the left
        outDx = (SCREEN_W * eased) / 100;
        inDx = outDx - SCREEN_W;
        break;
      default:
        break;
    }

    present(current, outDx, outDy, next, inDx, inDy);
  }
  current = next;
}

// Tapping an already-selected row activates it. For most menus the selection
// IS the setting, so activation just persists it; SETTINGS and IoT rows either
// open the keyboard on a field or run an action.
void activateItem(int menuId, uint8_t row) {
  Menu &m = menus[menuId];
  LOGF("activate: %s / %s\n", m.title, m.items[row]);

  if (menuId == MENU_SETTINGS) {
    switch (row) {
      case SET_NAME:
        kbOpen(nametagName, sizeof(nametagName), "NAMETAG", false, nullptr);
        slideTo(SCREEN_KB, G_UP);
        return;
      case SET_CLEAR:
        setField(nametagName, sizeof(nametagName), DEFAULT_NAME);
        saveSettings();  // deliberate and destructive; do not risk the debounce
        render();
        return;
    }
  }

  if (menuId == MENU_IOT) {
    switch (row) {
      case IOT_BROKER:
        kbOpen(iotBroker, sizeof(iotBroker), "MQTT BROKER", false,
               refreshIotLabels);
        break;
      case IOT_PORT:
        kbOpen(iotPort, sizeof(iotPort), "MQTT PORT", true, portCommitted);
        break;
      case IOT_USER:
        kbOpen(iotUser, sizeof(iotUser), "MQTT USER", false, refreshIotLabels);
        break;
      case IOT_TOPIC:
        kbOpen(iotTopic, sizeof(iotTopic), "MQTT TOPIC", false,
               refreshIotLabels);
        break;
      case IOT_STATUS:
        return;  // read-only until the MQTT client exists
      case IOT_ID:
        regenerateClientId();
        refreshIotLabels();
        markDirty();
        render();
        return;
    }
    slideTo(SCREEN_KB, G_UP);
    return;
  }

  if (menuId == MENU_SYSTEM) {
    switch (row) {
      case SYS_BRIGHT:
        // Tapping cycles up through the steps and wraps, so one row is the
        // whole control - no separate up/down affordance to fit on screen.
        brightness = (brightness + 1) % BRIGHT_COUNT;
        applyBrightness();
        refreshSysLabels();
        saveSettings();  // one deliberate tap; worth a flush
        render();
        return;
      case SYS_SSID:
        kbOpen(wifiSsid, sizeof(wifiSsid), "WIFI NETWORK", false,
               refreshSysLabels);
        break;
      case SYS_PASS:
        kbOpen(wifiPass, sizeof(wifiPass), "WIFI PASSWORD", false,
               refreshSysLabels);
        break;
      case SYS_STATUS:
        return;  // read-only; it reports, it does not act
      case SYS_CONNECT:
        if (wifiWanted) {
          wifiDisconnect();
        } else {
          wifiConnect();
        }
        refreshSysLabels();
        saveSettings();
        render();
        return;
    }
    slideTo(SCREEN_KB, G_UP);
    return;
  }

  // Everything else: the selection itself is the setting, so just persist it.
  markDirty();
}

// === Navigation ===
void handleGesture(Gesture g) {
  if (g == G_NONE) return;

  lastActivity = millis();

  // Any touch wakes the badge out of a running mode. A tap just dismisses it
  // to home. A swipe, though, carries intent - it was aimed at an edge - so
  // the mode is dropped and the gesture falls through to be handled normally,
  // opening the menu it was reaching for. Spending a deliberate swipe purely
  // on waking up meant every trip into a menu cost one extra gesture once the
  // badge had been sitting for IDLE_MS, which is most of the time.
  if (modeActive) {
    modeActive = false;
    current = SCREEN_HOME;
    navClear();  // running a mode ends the history; back from home is nowhere
    LOGF("mode exit (%s)\n", g == G_TAP ? "tap -> home" : "swipe, continuing");
    if (g == G_TAP) {
      render();
      return;
    }
  }

  // The keyboard owns every gesture while it is up.
  if (current == SCREEN_KB) {
    if (g == G_TAP) {
      switch (handleKeyboardTap(lastX, lastY)) {
        case KB_ENTRY: redrawKbEntry(); break;
        case KB_REDRAW: render(); break;
        case KB_DONE: slideTo(navPop(), G_DOWN); break;
        case KB_IGNORED: LOGF("kb tap %d,%d hit nothing\n", lastX, lastY); break;
      }
      return;
    }

    // Swiping back over the text deletes, which beats reaching for DEL in the
    // middle of a word and runs the same direction the caret moves.
    if (g == G_LEFT) {
      if (editPos > 0) {
        kbBackspace();
        redrawKbEntry();
      } else {
        LOGF("kb swipe LEFT at start of field, nothing to delete\n");
      }
      return;
    }

    // The keyboard slid up from the bottom, so pushing it back down dismisses
    // it - the same retreat-to-the-edge-it-came-from rule every menu follows.
    // Cancel used to be ANY swipe, which cannot coexist with swipe-to-delete.
    if (g == G_DOWN) {
      LOGF("edit '%s' cancelled\n", editTitle);
      slideTo(navPop(), G_DOWN);
      return;
    }

    LOGF("kb swipe %s ignored - LEFT deletes, DOWN cancels\n", gestureWord(g));
    return;
  }

  if (g == G_TAP) {
    // A tap during the splash means "I am here" - go straight to the summary
    // card instead of making the user wait out the timer.
    if (splashActive) {
      splashActive = false;
      render();
      return;
    }
    // Tap picks the row under the finger and redraws so there is immediate
    // feedback - without the redraw a working tap looks like a freeze.
    if (current >= 0) {
      Menu &m = menus[current];
      LOGF("tap at %d,%d\n", lastX, lastY);
      // Guarded rather than relying on the division: C truncates toward zero,
      // so a tap on the title bar above the list would otherwise land on row 0
      // and silently activate it.
      const int rel = lastY - menuRowY(0);
      const int row = rel / MENU_ROW_H;
      if (rel >= 0 && row < (int)m.count && lastY < NAV_Y) {
        const bool wasSelected = (row == m.index);
        if (!wasSelected) updateMenuSelection(m, (uint8_t)row);

        if (current == MENU_MODE) {
          // A mode is a choice, not an action. The first tap only moves the
          // highlight, so a mis-tap costs nothing; tapping the highlighted
          // row again confirms it and drops back to home showing the new
          // mode. Home then hands over to that mode on the next idle window.
          if (wasSelected) {
            saveSettings();  // deliberate commit: do not risk the debounce
            LOGF("mode confirmed: %s -> home\n", modeItems[activeMode()]);
            slideTo(navPop(), m.retreat);
          } else {
            LOGF("mode preview: %s\n", modeItems[activeMode()]);
          }
          return;
        }

        // Everywhere else one tap does the whole job. Requiring a tap to
        // select and a second to activate meant every choice cost two
        // gestures, and a row that happened to be highlighted already
        // behaved differently from one that was not.
        LOGF("select+activate: %s / %s\n", m.title, m.items[row]);
        activateItem(current, (uint8_t)row);
      } else {
        LOGF("tap outside list (row %d, y=%d)\n", row, lastY);
      }
    }
    return;
  }

  // Any deliberate navigation ends the splash, so returning home lands on the
  // summary card rather than the title card.
  splashActive = false;

  if (current == SCREEN_HOME) {
    // Which menu opens is decided by the edge the stroke STARTED at, not by
    // the direction it travelled. At the bottom edge a finger can only move
    // up, so direction alone would open the top menu - which is exactly the
    // mismatch this avoids. Swiping at an edge gives you that edge's menu.
    int target = -1;

    if (g == G_UP || g == G_DOWN) {
      if (startY <= EDGE_ZONE) {
        target = MENU_MODE;  // top edge
      } else if (startY >= SCREEN_H - EDGE_ZONE) {
        target = MENU_IOT;  // bottom edge
      } else {
        // Mid-screen: pull-from-edge semantics. Swiping up drags the bottom
        // menu into view, swiping down drags the top one.
        target = (g == G_UP) ? MENU_IOT : MENU_MODE;
      }
    } else {
      if (startX <= EDGE_ZONE) {
        target = MENU_SETTINGS;
      } else if (startX >= SCREEN_W - EDGE_ZONE) {
        target = MENU_SYSTEM;
      } else {
        target = (g == G_RIGHT) ? MENU_SETTINGS : MENU_SYSTEM;
      }
    }

    if (target >= 0) {
      LOGF("home swipe from %d,%d -> %s\n", startX, startY, menus[target].title);
      // The index is deliberately NOT reset here: it is the persisted
      // selection, so opening the MODE menu must show the mode that is
      // actually running, not snap back to the first row.
      navPush(SCREEN_HOME);
      slideTo(target, g);
    }
    return;
  }

  // Swipes move between screens; taps choose items. Nothing scrolls, because
  // nothing needs to: six rows is the longest list and all of them are on
  // screen at once. Tying a scroll direction to the edge a menu came from
  // produced three separate contradictions, all of which disappear here:
  //
  //   - the same physical swipe went opposite ways in different menus. Swiping
  //     down moved the highlight DOWN in MODE but UP in IoT CONFIG, because
  //     each menu's "forward" was whichever way it had been dragged in from.
  //   - the side menus ignored vertical swipes entirely, even though their
  //     lists are drawn vertically like every other menu's.
  //   - the nav bar named a gesture as the way out, but that gesture only
  //     actually left from the first row; anywhere else it moved the cursor.
  //     The bar promised an exit and usually delivered a scroll.
  //
  // Now the retreat swipe always leaves, exactly as the nav bar says.
  Menu &m = menus[current];
  if (g == m.retreat) {
    slideTo(navPop(), g);  // back to whatever screen opened this menu
  } else {
    LOGF("%s: swipe %s ignored - tap a row to choose, swipe %s to leave\n",
         m.title, gestureWord(g), gestureWord(m.retreat));
  }
}

void setup() {
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
  // Critical: with USB CDC, once the TX buffer fills and no host is reading,
  // every write blocks on this timeout. That is what makes the UI start snappy
  // and then progressively lag as log lines accumulate. 0 = never block, drop
  // output instead. Only HWCDC has this knob; a UART Serial cannot stall this
  // way, so the guard is what lets the sketch build with CDCOnBoot=default.
  Serial.setTxTimeoutMs(0);
#endif
  delay(200);
  Serial.println("badge booting");

  // Backlight on PWM so brightness is adjustable. If LEDC will not attach we
  // fall back to a plain high - a badge with a dim-control bug should still
  // light up, not sit there black.
  pinMode(TFT_BL, OUTPUT);
  blPwm = ledcAttach(TFT_BL, BL_FREQ, BL_RES);
  if (!blPwm) Serial.println("LEDC attach failed, backlight pinned on");
  digitalWrite(TFT_BL, HIGH);

  if (!panel->begin(SPI_SPEED)) {
    Serial.println("panel->begin() FAILED");
  } else {
    Serial.println("panel->begin() ok");
  }

  // The strip buffer is optional: if it will not allocate we still run, just
  // without animation. Never draw through a null framebuffer.
  stripReady = strip->begin(SPI_SPEED) && (strip->getFramebuffer() != nullptr);
  Serial.printf("strip buffer %s (%d bytes), free heap %lu, largest block %lu\n",
                stripReady ? "ok" : "UNAVAILABLE", SCREEN_W * STRIP_H * 2,
                (unsigned long)ESP.getFreeHeap(),
                (unsigned long)ESP.getMaxAllocHeap());

  Wire.begin(TP_SDA, TP_SCL, 400000);
  // Without a bus timeout a confused controller can stall the loop for a
  // second per poll, which reads as a lock-up.
  Wire.setTimeOut(8);
  touchReset();
  Serial.println("touch ready");

  loadSettings();
  applyBrightness();  // only meaningful once the stored level is known

  // If a network was configured on a previous boot, start joining it now.
  // Asynchronous, so this costs nothing at boot even if the AP is not there.
  if (wifiSsid[0]) {
    wifiConnect();
    refreshSysLabels();
  }

  splashStart = millis();
  lastActivity = millis();
  render();
}

void loop() {
  Gesture g = pollGesture();
  if (g != G_NONE) {
    uint32_t t0 = millis();
    handleGesture(g);
    LOGF("gesture %d -> screen %d, %lums\n", (int)g, current, millis() - t0);
  }

  const uint32_t now = millis();

  // Splash gives way to the home summary card.
  if (splashActive && now - splashStart >= SPLASH_MS) {
    splashActive = false;
    lastActivity = now;
    LOGF("splash done -> home\n");
    render();
  }

  // A menu left open on a lanyard should not stay open all weekend. Drop back
  // to home, which then hands over to the mode on the next idle window. The
  // keyboard is exempt: abandoning a half-typed name silently would be worse
  // than leaving it on screen.
  if (current >= 0 && now - lastActivity >= IDLE_MS) {
    LOGF("menu idle -> home\n");
    lastActivity = now;
    navClear();  // a timeout goes all the way home, not one step back
    slideTo(SCREEN_HOME, menus[current].retreat);
  }

  // Home left idle long enough hands over to the selected mode.
  if (!splashActive && !modeActive && current == SCREEN_HOME &&
      now - lastActivity >= IDLE_MS) {
    modeActive = true;
    slideIndex = 0;
    lastSlide = now;
    LOGF("idle -> mode %s\n", modeItems[activeMode()]);
    render();
  }

  // Flush settings once the user has stopped changing them.
  if (settingsDirty && now - lastChange >= SAVE_DEBOUNCE_MS) {
    saveSettings();
  }

  // Track the WiFi join without blocking on it. The labels only get rebuilt
  // when the status actually changes, and the screen is only redrawn when the
  // SYSTEM menu is the thing being looked at.
  if (now - lastWifiPoll >= WIFI_POLL_MS) {
    lastWifiPoll = now;
    const wl_status_t st = WiFi.status();
    if (st != lastWifiStatus) {
      lastWifiStatus = st;
      LOGF("wifi: %s\n", wifiStateText());
      refreshSysLabels();
      if (current == MENU_SYSTEM) render();
    }
  }

  // Advance the slideshow while it is the running mode.
  if (modeActive && activeMode() == MODE_SLIDESHOW &&
      now - lastSlide >= SLIDE_MS) {
    slideIndex++;
    lastSlide = now;
    render();
  }

  // Spin the lenticular cube. The angle is computed here, once per frame, so
  // that all four strips of a frame render the same pose - deriving it inside
  // the draw call would advance it mid-frame and shear the cube.
  if (modeActive && activeMode() == MODE_LENTICULAR &&
      now - lastFrame >= LENT_FRAME_MS) {
    lastFrame = now;
    lentAngle = (float)(now % LENT_PERIOD_MS) * (TWO_PI / LENT_PERIOD_MS);
    render();
  }

  delay(2);
}
