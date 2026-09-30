// power.cpp - see power.h.

#include "power.h"

#include <Arduino_GFX_Library.h>

#include "config.h"
#include "display.h"
#include "store.h"
#include "ui.h"

static uint16_t battMv = 0;
static uint32_t lastSample = 0;
static bool sampledOk = false;

// Rolling average. A single ADC read on this part is noisy, and a battery
// gauge that jitters by 10% between frames reads as broken even when the pack
// is fine.
static uint32_t accum = 0;
static uint8_t accumN = 0;

void powerBegin() {
  analogSetPinAttenuation(BATT_ADC_PIN, ADC_11db);  // full 0-~3.3V span
  lastSample = 0;
}

uint16_t batteryMillivolts() { return battMv; }

uint32_t uptimeSeconds() { return millis() / 1000UL; }

// A disconnected sense pin floats near 0 or rails high. Neither is a voltage a
// live pack could produce while the badge is still running, so anything outside
// a plausible window is reported as "no reading" rather than as a percentage.
bool batteryValid() {
  return sampledOk && battMv > 1500 && battMv < 6000;
}

int batteryPercent() {
  if (!batteryValid()) return -1;
  const int span = BATT_FULL_MV - BATT_EMPTY_MV;
  int pct = (int)(((int)battMv - BATT_EMPTY_MV) * 100 / span);
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  return pct;
}

void powerTick(uint32_t now) {
  if (now - lastSample < BATT_SAMPLE_MS / 4) return;
  lastSample = now;

  const uint32_t mv = (uint32_t)(analogReadMilliVolts(BATT_ADC_PIN) *
                                 BATT_DIVIDER);
  accum += mv;
  if (++accumN >= 4) {
    battMv = (uint16_t)(accum / accumN);
    accum = 0;
    accumN = 0;
    sampledOk = true;
  }
}

// A small outlined cell with a nub, filled proportionally. Drawn bottom-right
// so it sits clear of the mode captions along the bottom centre.
void drawBatteryIcon(Arduino_GFX *g, int ox, int oy, bool top) {
  const int w = 26, h = 12;
  const int x = ox + SCREEN_W - w - 6;
  const int y = top ? oy + 10 : oy + SCREEN_H - h - 6;
  const int pct = batteryPercent();

  // Backing box, so the icon stays legible over a busy lenticular frame.
  g->fillRect(x - 26, y - 2, w + 32, h + 4, C_BG);

  const uint16_t col = pct < 0 ? C_DIM : (pct <= 20 ? C_WARN : C_OK);
  g->drawRect(x, y, w, h, col);
  g->fillRect(x + w, y + 3, 2, h - 6, col);  // terminal nub

  if (pct >= 0) {
    const int fill = (w - 4) * pct / 100;
    if (fill > 0) g->fillRect(x + 2, y + 2, fill, h - 4, col);
    char txt[6];
    snprintf(txt, sizeof(txt), "%d%%", pct);
    g->setTextSize(1);
    g->setTextColor(col);
    g->setCursor(x - textWidth(txt, 1) - 4, y + 2);
    g->print(txt);
  } else {
    // No usable reading: say so, rather than showing a plausible-looking bar.
    g->setTextSize(1);
    g->setTextColor(C_DIM);
    g->setCursor(x - textWidth("--", 1) - 4, y + 2);
    g->print("--");
  }
}

// Rough steady-state draw for this badge. Numbers are order-of-magnitude for
// an ESP32-C3 + ST7789 pack — good for comparing settings, not for billing.
static int estimatedMilliamps() {
  int ma = 30;  // MCU + panel logic, radio off
  ma += (BRIGHT_PCT[brightness] * 75) / 100;  // backlight dominates
  if (wifiWanted) ma += 50;                   // STA on (idle or joining)
  if (modeActive) ma += 12;                   // continuous full-frame SPI
  return ma;
}

void drawPowerUsage(Arduino_GFX *g, int ox, int oy) {
  const int ma = estimatedMilliamps();
  char txt[10];
  snprintf(txt, sizeof(txt), "~%dmA", ma);

  const int tw = textWidth(txt, 1);
  const int x = ox + 6;
  const int y = oy + SCREEN_H - 12 - 6;

  g->fillRect(x - 2, y - 2, tw + 4, 14, C_BG);
  g->setTextSize(1);
  uint16_t col = C_OK;
  if (ma >= 120) col = C_WARN;
  else if (ma >= 80) col = C_NAV;
  else if (ma < 60) col = C_DIM;
  g->setTextColor(col);
  g->setCursor(x, y + 2);
  g->print(txt);
}
