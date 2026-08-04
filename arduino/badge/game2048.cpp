// game2048.cpp - a complete 2048 game driven by the badge's swipe gestures.

#include "game2048.h"
#include "config.h"
#include "display.h"

static uint16_t board[4][4];
static uint32_t score;
static bool gameOver;
static bool gameWon;

static void addRandom() {
  uint8_t empty[16];
  uint8_t count = 0;
  for (uint8_t r = 0; r < 4; r++)
    for (uint8_t c = 0; c < 4; c++)
      if (!board[r][c]) empty[count++] = r * 4 + c;
  if (!count) return;
  const uint8_t pick = empty[esp_random() % count];
  board[pick / 4][pick % 4] = (esp_random() % 10) < 9 ? 2 : 4;
}

static bool canMove() {
  for (uint8_t r = 0; r < 4; r++)
    for (uint8_t c = 0; c < 4; c++) {
      if (!board[r][c]) return true;
      if (c < 3 && board[r][c] == board[r][c + 1]) return true;
      if (r < 3 && board[r][c] == board[r + 1][c]) return true;
    }
  return false;
}

static bool slideRow(uint16_t row[4]) {
  bool moved = false;
  // Compact non-zero cells to the left.
  uint16_t tmp[4] = {};
  uint8_t pos = 0;
  for (uint8_t i = 0; i < 4; i++)
    if (row[i]) tmp[pos++] = row[i];
  // Merge adjacent equal cells.
  for (uint8_t i = 0; i < 3; i++) {
    if (tmp[i] && tmp[i] == tmp[i + 1]) {
      tmp[i] *= 2;
      score += tmp[i];
      if (tmp[i] == 2048) gameWon = true;
      for (uint8_t j = i + 1; j < 3; j++) tmp[j] = tmp[j + 1];
      tmp[3] = 0;
    }
  }
  for (uint8_t i = 0; i < 4; i++) {
    if (row[i] != tmp[i]) moved = true;
    row[i] = tmp[i];
  }
  return moved;
}

static bool doMove(Gesture g) {
  bool moved = false;
  if (g == G_LEFT) {
    for (uint8_t r = 0; r < 4; r++) moved |= slideRow(board[r]);
  } else if (g == G_RIGHT) {
    for (uint8_t r = 0; r < 4; r++) {
      uint16_t rev[4] = {board[r][3], board[r][2], board[r][1], board[r][0]};
      if (slideRow(rev)) {
        moved = true;
        for (uint8_t c = 0; c < 4; c++) board[r][c] = rev[3 - c];
      }
    }
  } else if (g == G_UP) {
    for (uint8_t c = 0; c < 4; c++) {
      uint16_t col[4] = {board[0][c], board[1][c], board[2][c], board[3][c]};
      if (slideRow(col)) {
        moved = true;
        for (uint8_t r = 0; r < 4; r++) board[r][c] = col[r];
      }
    }
  } else if (g == G_DOWN) {
    for (uint8_t c = 0; c < 4; c++) {
      uint16_t col[4] = {board[3][c], board[2][c], board[1][c], board[0][c]};
      if (slideRow(col)) {
        moved = true;
        for (uint8_t r = 0; r < 4; r++) board[r][c] = col[3 - r];
      }
    }
  }
  return moved;
}

void game2048Reset() {
  memset(board, 0, sizeof(board));
  score = 0;
  gameOver = false;
  gameWon = false;
  addRandom();
  addRandom();
}

// EXIT button geometry, shared between draw and hit-test.
#define EXIT_W 50
#define EXIT_H 28
#define EXIT_X (SCREEN_W - EXIT_W - 6)
#define EXIT_Y 4

bool game2048HandleGesture(Gesture g, int tapX, int tapY) {
  if (g == G_TAP) {
    // EXIT button in the top-right corner.
    if (tapX >= EXIT_X && tapY >= EXIT_Y && tapY < EXIT_Y + EXIT_H)
      return false;  // not consumed — let the mode-exit code handle it
    if (gameOver || gameWon) {
      game2048Reset();
      return true;
    }
    return true;  // consume all other taps
  }
  if (gameOver) return false;
  if (g != G_UP && g != G_DOWN && g != G_LEFT && g != G_RIGHT) return false;
  if (doMove(g)) {
    addRandom();
    if (!canMove()) gameOver = true;
    return true;
  }
  return false;
}

static uint16_t tileColor(uint16_t val) {
  switch (val) {
    case 2:    return 0xEF5D;  // warm white
    case 4:    return 0xEF1C;  // cream
    case 8:    return 0xFC40;  // orange
    case 16:   return 0xFB20;  // deeper orange
    case 32:   return 0xF980;  // red-orange
    case 64:   return 0xF800;  // red
    case 128:  return 0xEEC0;  // gold
    case 256:  return 0xEE40;  // deeper gold
    case 512:  return 0xEDC0;  // amber
    case 1024: return 0xED40;  // deep amber
    case 2048: return 0x07FF;  // cyan - you won
    default:   return 0x39C7;  // dark grey for >2048
  }
}

void drawGame2048(Arduino_GFX *g, int ox, int oy) {
  g->fillRect(ox, oy, SCREEN_W, SCREEN_H, C_BG);
  printCentered(g, "2048", ox, oy + 6, 2, C_ACCENT);

  // EXIT button, top-right.
  drawKey(g, ox + EXIT_X, oy + EXIT_Y, EXIT_W, EXIT_H, "EXIT", C_NAV, C_NAV_FG, 1);

  char buf[16];
  snprintf(buf, sizeof(buf), "score: %lu", (unsigned long)score);
  printCentered(g, buf, ox, oy + 28, 1, C_DIM);

  const int pad = 4;
  const int gridLeft = ox + 10;
  const int gridTop = oy + 44;
  const int cellW = (SCREEN_W - 20 - pad * 3) / 4;
  const int cellH = cellW;

  g->drawRect(gridLeft - 2, gridTop - 2,
              cellW * 4 + pad * 3 + 4, cellH * 4 + pad * 3 + 4, C_DIM);

  for (uint8_t r = 0; r < 4; r++) {
    for (uint8_t c = 0; c < 4; c++) {
      const int x = gridLeft + c * (cellW + pad);
      const int y = gridTop + r * (cellH + pad);
      const uint16_t val = board[r][c];
      const uint16_t bg = val ? tileColor(val) : 0x18C3;
      g->fillRect(x, y, cellW, cellH, bg);
      if (val) {
        snprintf(buf, sizeof(buf), "%u", (unsigned)val);
        const uint8_t sz = val < 100 ? 2 : (val < 1000 ? 1 : 1);
        const int tw = textWidth(buf, sz);
        const int tx = x + (cellW - tw) / 2;
        const int ty = y + (cellH - GLYPH_H * sz) / 2;
        g->setTextSize(sz);
        g->setTextColor(val >= 8 ? 0xFFFF : 0x0000);
        g->setCursor(tx, ty);
        g->print(buf);
      }
    }
  }

  const int footY = gridTop + cellH * 4 + pad * 3 + 8;
  if (gameWon) {
    printCentered(g, "YOU WIN!", ox, footY, 2, C_OK);
    printCentered(g, "tap to restart", ox, footY + 22, 1, C_DIM);
  } else if (gameOver) {
    printCentered(g, "GAME OVER", ox, footY, 2, C_WARN);
    printCentered(g, "tap to restart", ox, footY + 22, 1, C_DIM);
  } else {
    printCentered(g, "swipe to play", ox,
                  oy + SCREEN_H - 18, 1, C_DIM);
  }
}
