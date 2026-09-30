// keyboard.cpp - see keyboard.h.

#include "keyboard.h"

#include "display.h"
#include "input.h"
#include "menus.h"   // navPush: opening the keyboard is a navigation step
#include "store.h"
#include "ui.h"      // current

// `editTarget` is the buffer the commit writes back into.
static char *editTarget = nullptr;
static size_t editTargetSize = 0;
static const char *editTitle = "";
static bool editNumeric = false;
static void (*editOnCommit)() = nullptr;

static char editBuf[EDIT_MAX];
static uint8_t editLen = 0;
static uint8_t editPos = 0;  // insertion caret, 0..editLen
static KbCase kbCase = KC_LOWER;
static bool kbSymbols = false;

const char *kbTitle() { return editTitle; }

// Two pages. The letter page keeps . - _ inline because a hostname needs them
// constantly and a page flip per dot would be miserable; the symbol page then
// covers all 32 ASCII punctuation marks, in ASCII order, 8 per row. Between
// the two, every printable ASCII character is reachable.
static const char *const KB_ROWS_ALPHA[] = {"1234567890", "qwertyuiop", "asdfghjkl",
                                      "zxcvbnm.-_"};
static const char *const KB_ROWS_SYM[] = {"!\"#$%&'(", ")*+,-./:", ";<=>?@[\\",
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

// Action row: CANCEL | DEL | case | page | SPACE | OK.
#define KB_CANCEL_W 48
#define KB_DEL_W 38
#define KB_CASE_W 32
#define KB_PAGE_W 32
#define KB_OK_W 40
#define KB_SPACE_W \
  (SCREEN_W - KB_CANCEL_W - KB_DEL_W - KB_CASE_W - KB_PAGE_W - KB_OK_W)

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
  snprintf(hint, sizeof(hint), "%s del  %s cancel", gestureArrow(G_LEFT),
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

  // Action row: CANCEL | DEL | case | page | SPACE | OK. The case key shows the case
  // you are about to type, so it reads as state rather than as a command.
  const int ay = oy + KB_ACTION_Y;
  int x = ox;
  drawKey(g, x, ay, KB_CANCEL_W, KB_ACTION_H, "CANCEL", C_NAV, C_NAV_FG, 1);
  x += KB_CANCEL_W;
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
    if (x < KB_CANCEL_W) return KB_CANCEL;
    if (x < KB_CANCEL_W + KB_DEL_W) {
      if (editPos == 0) return KB_IGNORED;
      kbBackspace();
      return KB_ENTRY;
    }
    if (x >= SCREEN_W - KB_OK_W) {
      kbCommit();
      return KB_DONE;
    }
    if (editNumeric) return KB_IGNORED;  // inert filler where the rest would be
    if (x < KB_CANCEL_W + KB_DEL_W + KB_CASE_W) {
      if (kbSymbols) return KB_IGNORED;  // punctuation has no case
      kbCase = (KbCase)((kbCase + 1) % 3);
      return KB_REDRAW;  // every letter key changes case
    }
    if (x < KB_CANCEL_W + KB_DEL_W + KB_CASE_W + KB_PAGE_W) {
      kbSymbols = !kbSymbols;
      return KB_REDRAW;  // the whole grid is replaced
    }
    kbInsert(' ');
    return KB_ENTRY;
  }
  return KB_IGNORED;
}

// Swipe-to-delete, driven from ui.cpp. Returns false when there is nothing to
// the left of the caret, so the caller can log the no-op rather than repaint.
bool kbDeleteAtCaret() {
  if (editPos == 0) return false;
  kbBackspace();
  redrawKbEntry();
  return true;
}
