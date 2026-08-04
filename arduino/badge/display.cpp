// display.cpp - see display.h.

#include "display.h"

#include "input.h"
#include "keyboard.h"
#include "menus.h"
#include "power.h"
#include "modes.h"
#include "store.h"
#include "ui.h"

Arduino_DataBus *bus = new Arduino_ESP32SPI(
    TFT_DC, GFX_NOT_DEFINED /* CS tied to GND */, TFT_SCK, TFT_MOSI,
    GFX_NOT_DEFINED /* MISO */);

Arduino_GFX *panel = new Arduino_ST7789(
    bus, TFT_RST, 0 /* rotation */, true /* IPS */, SCREEN_W, SCREEN_H);

Arduino_Canvas *strip = new Arduino_Canvas(SCREEN_W, STRIP_H, panel);
bool stripReady = false;
Gesture pendingGesture = G_NONE;

// LEDC drives the backlight so brightness is adjustable. If it will not attach
// we fall back to a plain digital high - a bug in the dimmer should leave the
// user at full brightness, never staring at a black screen.
static bool blPwm = false;

bool displayBegin() {
  pinMode(TFT_BL, OUTPUT);
  blPwm = ledcAttach(TFT_BL, BL_FREQ, BL_RES);
  if (!blPwm) Serial.println("LEDC attach failed, backlight pinned on");
  digitalWrite(TFT_BL, HIGH);

  const bool ok = panel->begin(SPI_SPEED);
  Serial.printf("panel->begin() %s\n", ok ? "ok" : "FAILED");
  if (displayFlipped) panel->setRotation(2);

  // The strip buffer is optional: if it will not allocate we still run, just
  // without animation. Never draw through a null framebuffer.
  stripReady = strip->begin(SPI_SPEED) && (strip->getFramebuffer() != nullptr);
  Serial.printf("strip buffer %s (%d bytes), free heap %lu, largest block %lu\n",
                stripReady ? "ok" : "UNAVAILABLE", SCREEN_W * STRIP_H * 2,
                (unsigned long)ESP.getFreeHeap(),
                (unsigned long)ESP.getMaxAllocHeap());
  return ok;
}

void displayApplyBrightness() {
  if (brightness >= BRIGHT_COUNT) brightness = BRIGHT_COUNT - 1;
  if (blPwm) {
    ledcWrite(TFT_BL, (BRIGHT_PCT[brightness] * 255) / 100);
  } else {
    digitalWrite(TFT_BL, HIGH);
  }
}

void displayFlip() {
  displayFlipped = !displayFlipped;
  panel->setRotation(displayFlipped ? 2 : 0);
  markDirty();
}

// === Primitives ===
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

// === Compositing ===
// drawn - no ambient state - so a transition can composite any two screens.
void drawScreen(Arduino_GFX *g, int id, int ox, int oy) {
  // A banner outranks everything: it is the one thing an operator can put on
  // every badge at once, and it should not be second-guessed by local state.
  if (bannerActive()) {
    drawBanner(g, ox, oy);
    return;
  }
  if (id == SCREEN_KB) {
    drawKeyboard(g, ox, oy);
  } else if (id == SCREEN_CREDITS) {
    drawCredits(g, ox, oy);
    return;  // no battery gauge: the credits are a full-bleed card
  } else if (id == SCREEN_HOME) {
    if (modeActive) {
      drawModeScreen(g, ox, oy);
    } else {
      drawHome(g, ox, oy);
    }
  } else {
    drawMenu(g, menus[id], ox, oy);
    if (showBatteryIcon) drawBatteryIcon(g, ox, oy, true);
    return;
  }

  // Only over home and the modes: a menu has its own nav bar in that corner,
  // and the gauge would sit on top of it.
  const bool scanner = id == SCREEN_HOME && modeActive &&
                       activeMode() == MODE_WIFI_SCAN;
  if (showBatteryIcon && !scanner) drawBatteryIcon(g, ox, oy);
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

    // Poll touch between strips rather than only between frames. A redraw is
    // ~40ms of solid SPI, and with every mode animated that dropped the poll
    // rate from ~500/s to ~25/s - a brisk tap could land entirely inside one
    // frame and never be seen. Any gesture that completes mid-render is
    // stashed for loop() to handle, because acting on it here would re-enter
    // the compositor we are standing in.
    const Gesture g = pollGesture();
    if (g != G_NONE && pendingGesture == G_NONE) pendingGesture = g;

    yield();  // a full redraw is ~1.5MB of SPI; keep the task WDT fed
  }
}

void render() { present(current, 0, 0, current, 0, 0); }

// A selection change updates two small rectangles plus the nav counter,

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
