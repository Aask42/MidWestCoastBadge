// ui.cpp - see ui.h.

#include "ui.h"

#include "display.h"
#include "identity.h"
#include "input.h"
#include "keyboard.h"
#include "menus.h"
#include "modes.h"
#include "ble.h"
#include "net.h"
#include "store.h"

int current = SCREEN_HOME;
bool splashActive = true;
bool modeActive = false;
uint32_t lastActivity = 0;

static uint32_t splashStart = 0;
static uint32_t splashLastFrame = 0;
static float splashPhase = 0.0f;
static float homePhase = 0.0f;

static char bannerText[96] = "";
static uint32_t bannerUntil = 0;

bool bannerActive() { return bannerUntil && millis() < bannerUntil; }

void bannerShow(const char *text, uint32_t seconds) {
  snprintf(bannerText, sizeof(bannerText), "%s", text);
  bannerUntil = millis() + seconds * 1000UL;
  LOGF("banner: '%s' for %lus\n", bannerText, (unsigned long)seconds);
  render();
}

// Word-wrapped and scaled to fill, because a message nobody can read across a
// room is not much of a message. Same fitting idea as the nametag.
void drawBanner(Arduino_GFX *g, int ox, int oy) {
  g->fillRect(ox, oy, SCREEN_W, SCREEN_H, C_NAV);
  g->fillRect(ox + 6, oy + 6, SCREEN_W - 12, SCREEN_H - 12, C_BG);

  const int maxW = SCREEN_W - 20;
  // Break into lines that fit at the largest size where the whole message
  // still fits vertically. Try big first and walk down.
  for (int size = 6; size >= 1; size--) {
    const int cpl = maxW / (GLYPH_W * size);          // chars per line
    if (cpl < 4) continue;
    const int lh = GLYPH_H * size + 4;
    char lines[8][40];
    int n = 0;
    const char *p = bannerText;
    while (*p && n < 8) {
      int take = (int)strlen(p);
      if (take > cpl) {
        take = cpl;
        // Prefer a space so words are not split mid-air.
        int b = take;
        while (b > 0 && p[b] != ' ') b--;
        if (b > 0) take = b;
      }
      snprintf(lines[n], sizeof(lines[0]), "%.*s", take, p);
      n++;
      p += take;
      while (*p == ' ') p++;
    }
    if (*p) continue;                                  // did not fit in 8 lines
    if (n * lh > SCREEN_H - 40) continue;              // too tall
    const int top = oy + (SCREEN_H - n * lh) / 2;
    for (int i = 0; i < n; i++) {
      printCentered(g, lines[i], ox, top + i * lh, (uint8_t)size, C_NAV);
    }
    return;
  }
}

// QR code for https://midwestcoast.best/badge — 25x25 modules, packed 1bpp.
static const uint8_t QR_SIZE = 25;
static const uint8_t QR_DATA[] PROGMEM = {
  0xFE,0x66,0x3F,0x80, 0x82,0x68,0xA0,0x80, 0xBA,0x62,0xAE,0x80,
  0xBA,0xCD,0xAE,0x80, 0xBA,0xD2,0xAE,0x80, 0x82,0x24,0xA0,0x80,
  0xFE,0xAA,0xBF,0x80, 0x00,0x2B,0x00,0x00, 0xC7,0x4F,0x0C,0x00,
  0x71,0x71,0x1F,0x00, 0xBF,0xAB,0x15,0x80, 0x29,0x58,0xFC,0x80,
  0x96,0xDB,0x30,0x80, 0xD1,0x15,0x91,0x00, 0xAE,0xE7,0xFD,0x80,
  0xAC,0x33,0xB6,0x80, 0xB2,0x1F,0xFA,0x00, 0x00,0xF9,0x88,0x00,
  0xFE,0x9A,0xA8,0x80, 0x82,0xA1,0x89,0x00, 0xBA,0x47,0xFB,0x00,
  0xBA,0x73,0xE1,0x80, 0xBA,0x42,0x46,0x80, 0x82,0x9A,0xF8,0x80,
  0xFE,0x90,0x84,0x80,
};

static void drawQr(Arduino_GFX *g, int cx, int cy, uint8_t scale) {
  const int total = QR_SIZE * scale;
  const int x0 = cx - total / 2;
  const int y0 = cy - total / 2;
  // White background with 1-module quiet zone.
  g->fillRect(x0 - scale, y0 - scale, total + scale * 2, total + scale * 2, 0xFFFF);
  const int bytesPerRow = (QR_SIZE + 7) / 8;
  for (uint8_t r = 0; r < QR_SIZE; r++) {
    for (uint8_t c = 0; c < QR_SIZE; c++) {
      if (pgm_read_byte(&QR_DATA[r * bytesPerRow + c / 8]) & (1 << (7 - (c & 7))))
        g->fillRect(x0 + c * scale, y0 + r * scale, scale, scale, 0x0000);
    }
  }
}

// The credits. Reached from the bottom swipe or by holding on the home screen.
void drawCredits(Arduino_GFX *g, int ox, int oy) {
  g->fillRect(ox, oy, SCREEN_W, SCREEN_H, C_BG);
  g->drawRect(ox + 6, oy + 6, SCREEN_W - 12, SCREEN_H - 12, C_ACCENT);
  g->drawRect(ox + 7, oy + 7, SCREEN_W - 14, SCREEN_H - 14, C_ACCENT);

  printCentered(g, "Made by", ox, oy + 16, 1, C_DIM);
  printCentered(g, CREDITS_NAME1, ox, oy + 30, 2, C_FG);
  printCentered(g, CREDITS_NAME2, ox, oy + 52, 2, C_FG);
  printCentered(g, CREDITS_FOR, ox, oy + 74, 1, C_DIM);
  printCentered(g, CREDITS_AKA, ox, oy + 88, 1, C_ACCENT);

  drawQr(g, ox + SCREEN_W / 2, oy + 180, 5);

  printCentered(g, "midwestcoast.best/badge", ox, oy + SCREEN_H - 42, 1, C_NAV);
  printCentered(g, "v" BADGE_VERSION "  touch to return", ox, oy + SCREEN_H - 26, 1, C_DIM);
}

// The four swipe affordances drawn around the edge of the home screen.

void drawHomeHints(Arduino_GFX *g, int ox, int oy) {
  // Each hint sits on the edge you swipe from, and its arrow points the way
  // that edge's menu is dragged in: down from the top, up from the bottom.
  char buf[24];

  snprintf(buf, sizeof(buf), "Mode %s", gestureArrow(G_DOWN));
  printCentered(g, buf, ox, oy + 10, 1, C_FG);

  snprintf(buf, sizeof(buf), "Credits %s", gestureArrow(G_UP));
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

static void splashLineParity(Arduino_GFX *g, int x0, int y0, int x1, int y1,
                             uint16_t color, int parity) {
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (;;) {
    if ((x0 & 1) == parity) g->drawPixel(x0, y0, color);
    if (x0 == x1 && y0 == y1) break;
    const int twice = 2 * err;
    if (twice >= dy) {
      err += dy;
      x0 += sx;
    }
    if (twice <= dx) {
      err += dx;
      y0 += sy;
    }
  }
}

static void splashFrame(Arduino_GFX *g, int ox, int oy, float scale,
                        int disparity, uint16_t color, int parity) {
  const int cx = ox + SCREEN_W / 2 + (parity ? disparity : -disparity);
  const int cy = oy + SCREEN_H / 2;
  const int halfW = (int)(scale * 108.0f);
  const int halfH = (int)(scale * 142.0f);
  splashLineParity(g, cx - halfW, cy - halfH, cx + halfW, cy - halfH,
                   color, parity);
  splashLineParity(g, cx + halfW, cy - halfH, cx + halfW, cy + halfH,
                   color, parity);
  splashLineParity(g, cx + halfW, cy + halfH, cx - halfW, cy + halfH,
                   color, parity);
  splashLineParity(g, cx - halfW, cy + halfH, cx - halfW, cy - halfH,
                   color, parity);
}

static void drawSplash(Arduino_GFX *g, int ox, int oy) {
  g->fillRect(ox, oy, SCREEN_W, SCREEN_H, C_BG);

  for (int i = 0; i < 8; i++) {
    const float depth = fmodf((float)i / 8.0f + splashPhase, 1.0f);
    const float scale = 0.12f + depth * depth * 0.88f;
    const int disparity = 1 + (int)((1.0f - depth) * 5.0f);
    splashFrame(g, ox, oy, scale, disparity, C_ACCENT, 0);
    splashFrame(g, ox, oy, scale, disparity, C_NAV, 1);
  }

  // A quiet band keeps the identity readable while the stereo tunnel moves
  // through and behind it.
  g->fillRect(ox, oy + 112, SCREEN_W, 96, C_BG);
  printCentered(g, "MIDWESTCOAST", ox, oy + 122, 2, C_FG);
  printCentered(g, "2026", ox, oy + 151, 4, C_ACCENT);
  printCentered(g, "DEF CON 34", ox, oy + 190, 1, C_NAV);
  printCentered(g, "By: Aask", ox, oy + 210, 1, C_FG);
  printCentered(g, "swipe to navigate", ox, oy + 236, 1, C_DIM);
  printCentered(g, "v" BADGE_VERSION, ox, oy + SCREEN_H - 18, 1, C_DIM);
}

void drawHome(Arduino_GFX *g, int ox, int oy) {
  g->fillRect(ox, oy, SCREEN_W, SCREEN_H, C_BG);

  if (splashActive) {
    drawSplash(g, ox, oy);
    return;  // splash stays clean: no edge hints competing with the title
  }

  printCentered(g, "DEF CON 34", ox, oy + 100, 2, C_ACCENT);
  printCentered(g, "MODE", ox, oy + 138, 1, C_DIM);
  printCentered(g, modeItems[activeMode()], ox, oy + 156, 2, C_FG);
  printCentered(g, "tap to show", ox, oy + 178, 1, C_ACCENT);

  // Animated ring behind the mode label.
  const int cx = ox + SCREEN_W / 2;
  const int cy = oy + 150;
  const float pulse = 0.5f + 0.5f * sinf(homePhase * TWO_PI);
  const int r = 52 + (int)(pulse * 8.0f);
  const uint8_t bright = (uint8_t)(40 + pulse * 30);
  const uint16_t ring = ((bright >> 3) << 11) | ((bright >> 2) << 5) | (bright >> 3);
  g->drawCircle(cx, cy, r, ring);
  g->drawCircle(cx, cy, r + 1, ring);

  // Status strip. Whether the badge is on the network, and who it is, are the
  // two things worth being able to check at a glance rather than by diving
  // three swipes deep into a menu.
  char line[40];
  snprintf(line, sizeof(line), "wifi %s", wifiStateText());
  printCentered(g, line, ox, oy + 196, 1,
                wifiIsConnected() ? C_OK : C_DIM);

  const uint8_t nearby = bleNearbyCount();
  if (nearby) {
    snprintf(line, sizeof(line), "%u badge%s nearby", (unsigned)nearby,
             nearby == 1 ? "" : "s");
    printCentered(g, line, ox, oy + 208, 1, C_OK);
  }

  snprintf(line, sizeof(line), "id %s  secret %s", badgeId, badgeCode);
  printCentered(g, line, ox, oy + 222, 1, C_DIM);

  drawHomeHints(g, ox, oy);
}

// The nav bar names the exact swipe that leaves this menu, so the way out is

// === Navigation ===
void handleGesture(Gesture g) {
  if (g == G_NONE) return;

  lastActivity = millis();

  // Credits are modal: they went up on a deliberate hold, so anything at all
  // takes them down again and nothing falls through. Checked first so the
  // gesture cannot also be read as navigation on the way out.
  if (current == SCREEN_CREDITS) {
    LOGF("credits dismissed (%s)\n", gestureWord(g));
    slideTo(SCREEN_HOME, G_DOWN);  // slideTo sets current; it needs the old one
    return;
  }

  // A hold anywhere that is not a menu or the keyboard opens the credits. Menus
  // are excluded because a finger resting on a row while someone reads the list
  // is ordinary behaviour there, and the keyboard because it is mid-edit.
  if (g == G_HOLD) {
    if (current == SCREEN_HOME) {
      splashActive = false;
      modeActive = false;
      navClear();
      LOGF("hold -> credits\n");
      slideTo(SCREEN_CREDITS, G_UP);
    } else {
      LOGF("hold on screen %d ignored\n", current);
    }
    return;
  }

  // Any touch wakes the badge out of a running mode. A tap just dismisses it
  // to home. A swipe, though, carries intent - it was aimed at an edge - so
  // the mode is dropped and the gesture falls through to be handled normally,
  // opening the menu it was reaching for. Spending a deliberate swipe purely
  // on waking up meant every trip into a menu cost one extra gesture once the
  // badge had been sitting for MODE_IDLE_MS, which is most of the time.
  if (modeActive) {
    if (modesHandleGesture(g)) {
      render();
      return;
    }
    modesExit();
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
        case KB_CANCEL:
          LOGF("edit '%s' cancelled by button\n", kbTitle());
          slideTo(navPop(), G_DOWN);
          break;
        case KB_IGNORED: LOGF("kb tap %d,%d hit nothing\n", lastX, lastY); break;
      }
      return;
    }

    // Swiping back over the text deletes, which beats reaching for DEL in the
    // middle of a word and runs the same direction the caret moves.
    if (g == G_LEFT) {
      if (!kbDeleteAtCaret()) {
        LOGF("kb swipe LEFT at start of field, nothing to delete\n");
      }
      return;
    }

    // The keyboard slid up from the bottom, so pushing it back down dismisses
    // it - the same retreat-to-the-edge-it-came-from rule every menu follows.
    // Cancel used to be ANY swipe, which cannot coexist with swipe-to-delete.
    if (g == G_DOWN) {
      LOGF("edit '%s' cancelled\n", kbTitle());
      slideTo(navPop(), G_DOWN);
      return;
    }

    LOGF("kb swipe %s ignored - LEFT deletes, DOWN cancels\n", gestureWord(g));
    return;
  }

  if (g == G_TAP) {
    // Tapping the idle screen runs whatever the home card is showing. The card
    // already names the selected mode in its centre, so this reads as "show me
    // that" rather than as a hidden shortcut - and it works from the splash
    // too, so a tap never just means "wait less".
    //
    // Combined with tap-to-leave-a-mode above, tap is a clean toggle:
    // home -> mode -> home.
    if (current == SCREEN_HOME) {
      splashActive = false;
      modeActive = true;
      modesEnter(millis());
      LOGF("tap on home -> mode %s\n", modeItems[activeMode()]);
      render();
      return;
    }
    // Tap picks the row under the finger and redraws so there is immediate
    // feedback - without the redraw a working tap looks like a freeze.
    if (current >= 0) {
      Menu &m = menus[current];
      LOGF("tap at %d,%d\n", lastX, lastY);
      if (lastY >= NAV_Y) {
        // FLIP button in the nav bar's right corner.
        if (lastX >= SCREEN_W - 46) {
          displayFlip();
          render();
          return;
        }
        menusDisarmReset();
        LOGF("%s: BACK tapped\n", m.title);
        slideTo(navPop(), m.retreat);
        return;
      }
      // Guarded rather than relying on the division: C truncates toward zero,
      // so a tap on the title bar above the list would otherwise land on row 0
      // and silently activate it.
      const int rel = lastY - menuRowY(0);
      const int row = rel >= 0 ? menuRowAtSlot(m, rel / MENU_ROW_H) : -1;
      if (row >= 0 && lastY < NAV_Y) {
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

  menusDisarmReset();  // an armed wipe never survives leaving its screen

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
        navPush(SCREEN_HOME);
        slideTo(SCREEN_CREDITS, G_UP);
        return;
      } else {
        // Mid-screen: pull-from-edge semantics. Swiping up drags the bottom
        // menu into view, swiping down drags the top one.
        if (g == G_UP) {
          navPush(SCREEN_HOME);
          slideTo(SCREEN_CREDITS, G_UP);
          return;
        }
        target = MENU_MODE;
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
  } else if (g == gestureOpposite(m.retreat) && menuPageForward(m)) {
    // Dragging further in the direction the menu came from shows more of it.
    // This only does anything when the menu is too long for one screen, and it
    // moves the PAGE, never the highlight - taps still choose.
    LOGF("%s: page %u\n", m.title, (unsigned)m.page + 1);
    render();
  } else {
    LOGF("%s: swipe %s ignored - tap a row to choose, swipe %s to leave\n",
         m.title, gestureWord(g), gestureWord(m.retreat));
  }
}

// Splash expiry, idle handover to a mode, and the menu timeout. Split out of
// loop() so that badge.ino stays a wiring diagram rather than a state machine.
void uiTick(uint32_t now) {
  if (splashStart == 0) splashStart = now;

  if (splashActive && now - splashLastFrame >= 100) {
    splashLastFrame = now;
    splashPhase = fmodf(splashPhase + 0.025f, 1.0f);
    render();
  }

  // A banner expiring puts the badge back to whatever it was doing.
  static bool bannerWas = false;
  const bool bannerNow = bannerActive();
  if (bannerWas && !bannerNow) {
    LOGF("banner expired\n");
    render();
  }
  bannerWas = bannerNow;
  if (bannerNow) return;  // nothing else runs while a banner is up

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
  if (current >= 0 && now - lastActivity >= MENU_IDLE_MS) {
    LOGF("menu idle -> home\n");
    lastActivity = now;
    navClear();  // a timeout goes all the way home, not one step back
    slideTo(SCREEN_HOME, menus[current].retreat);
  }

  if (current == SCREEN_KB && now - lastActivity >= EDIT_IDLE_MS) {
    LOGF("edit '%s' idle -> cancelled\n", kbTitle());
    lastActivity = now;
    slideTo(navPop(), G_DOWN);
  }

  // Credits time out like a menu does. Neither the menu timeout above nor the
  // mode handover below catches this screen - one wants a menu id, the other
  // wants home - so a badge whose owner walked away mid-hold would otherwise
  // sit on the credits until someone touched it.
  if (current == SCREEN_CREDITS && now - lastActivity >= MENU_IDLE_MS) {
    LOGF("credits idle -> home\n");
    lastActivity = now;
    slideTo(SCREEN_HOME, G_DOWN);
  }

  // Home left idle long enough hands over to the selected mode.
  if (!splashActive && !modeActive && current == SCREEN_HOME &&
      now - lastActivity >= MODE_IDLE_MS) {
    modeActive = true;
    modesEnter(now);
    LOGF("idle -> mode %s\n", modeItems[activeMode()]);
    render();
  }

  // Animate the idle home card before mode handover.
  static uint32_t homeLastFrame = 0;
  if (!splashActive && !modeActive && current == SCREEN_HOME &&
      now - homeLastFrame >= 80) {
    homeLastFrame = now;
    homePhase = fmodf(homePhase + 0.012f, 1.0f);
    render();
  }

  // Animated modes drive their own repaints.
  if (modeActive && modesTick(now)) render();
}
