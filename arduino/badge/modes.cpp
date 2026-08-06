// modes.cpp - see modes.h.

#include "modes.h"

#include "ble.h"
#include "display.h"
#include "game2048.h"
#include "input.h"
#include "menus.h"
#include <math.h>
#include <sys/time.h>

#include <LittleFS.h>

#include "store.h"
#include "mqtt.h"
#include "net.h"
#include "ui.h"

static uint8_t slideIndex = 0;
static uint32_t lastFrame = 0;
static uint32_t scanRefreshAt = 0;
static uint8_t scanOffset = 0;

#define WIFI_SCAN_VISIBLE 5

static uint8_t bleOffset = 0;
#define BLE_SCAN_VISIBLE 6

// Shared animation phase. Advanced once per frame in modesTick() — never from
// millis() inside a draw, or strips shear. Declared early so the nametag can
// ride the same clock as the lenticular scenes.
float lentAngle = 0.0f;

#define LENT_MATRIX 4     // rain fall-throughs per animation loop
#define LENT_STARS 28     // starfield depth cycles per lenticular loop
#define NAME_BG_MS 10000  // how long each nametag background scene stays

static uint8_t scanMaxOffset() {
  const uint8_t count = wifiScanCount();
  return count > WIFI_SCAN_VISIBLE ? count - WIFI_SCAN_VISIBLE : 0;
}

// === Mode rendering ===
// Stand-ins until real assets exist. Everything is procedural so it costs no
// flash. That mattered more before WiFi: linking it took the build from ~400KB
// to ~1.05MB, which is 80% of the default 1.2MB app partition. Build with
// PartitionScheme=huge_app (3MB app) before adding real image assets.

// Nametag name rendering (rainbow / solid, outlined).
static uint16_t hsv565(uint8_t h, uint8_t s, uint8_t v) {
  // h sweeps 0..255 around the wheel. Integer path so it stays cheap per glyph.
  if (s == 0) {
    const uint8_t g = v;
    return (uint16_t)(((g & 0xF8) << 8) | ((g & 0xFC) << 3) | (g >> 3));
  }
  const uint8_t region = (uint8_t)(h / 43);           // 0..5
  const uint8_t rem = (uint8_t)((h - region * 43) * 6);  // 0..258≈
  const uint16_t p = (uint16_t)(v * (255 - s)) >> 8;
  const uint16_t q =
      (uint16_t)(v * (255 - ((uint16_t)s * rem >> 8))) >> 8;
  const uint16_t t =
      (uint16_t)(v * (255 - ((uint16_t)s * (255 - rem) >> 8))) >> 8;
  uint8_t r, g, b;
  switch (region) {
    case 0:  r = v; g = (uint8_t)t; b = (uint8_t)p; break;
    case 1:  r = (uint8_t)q; g = v; b = (uint8_t)p; break;
    case 2:  r = (uint8_t)p; g = v; b = (uint8_t)t; break;
    case 3:  r = (uint8_t)p; g = (uint8_t)q; b = v; break;
    case 4:  r = (uint8_t)t; g = (uint8_t)p; b = v; break;
    default: r = v; g = (uint8_t)p; b = (uint8_t)q; break;
  }
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Rainbow keyed to horizontal position so multi-line names share one left→right
// spectrum. Phase subtracts so the colours flow across the panel (L→R).
static uint16_t nameFillColor(int letterX, int cellW) {
  if (nametagColor != NAME_COLOR_RAINBOW) {
    return NAME_COLOR_RGB[nametagColor % NAME_COLOR_COUNT];
  }
  const int midX = letterX + cellW / 2;
  // One full wheel across the panel width.
  const int spatial = (midX * 255) / SCREEN_W;
  // Six flows per lenticular loop (~1 Hz at the usual 6s yaw period).
  const int phase =
      (int)((lentAngle / TWO_PI) * 255.0f * 6.0f);
  const uint8_t h = (uint8_t)(spatial - phase);
  return hsv565(h, 255, 255);
}

static void drawNameFitted(Arduino_GFX *g, int ox, int oy, const char *name,
                           int bandTop, int bandH, uint16_t color, int dx,
                           int dy, bool perLetter) {
  if (!name || !name[0]) return;

  char words[4][20];
  int lens[4] = {0};
  int nWords = 0;
  const char *p = name;
  while (*p && nWords < 4) {
    while (*p == ' ') p++;
    if (!*p) break;
    int i = 0;
    while (*p && *p != ' ' && i < (int)sizeof(words[0]) - 1) {
      words[nWords][i++] = *p++;
    }
    words[nWords][i] = '\0';
    if (i > 0) {
      lens[nWords] = i;
      nWords++;
    }
    while (*p && *p != ' ') p++;  // drop overflow past the 19-char word cap
  }
  if (nWords == 0) return;

  const int maxW = SCREEN_W - 8;
  // Each space in the name becomes a blank line gap between word rows.
  const int gap = (nWords > 1) ? 16 : 0;
  // First pass: width-ideal size per line (uncapped by height).
  int sizes[4];
  int sumH = gap * (nWords - 1);
  for (int i = 0; i < nWords; i++) {
    int s = maxW / (GLYPH_W * lens[i]);
    if (s < 1) s = 1;
    if (s > 24) s = 24;
    sizes[i] = s;
    sumH += GLYPH_H * s;
  }
  // If the stack is too tall, scale every line down by the same factor so
  // relative "fill the width" intent is preserved as much as height allows.
  if (sumH > bandH) {
    // Integer scale: find the largest k/256 such that sum stays in band.
    int scaled = 0;
    for (int k = 256; k >= 1; k--) {
      int h = gap * (nWords - 1);
      for (int i = 0; i < nWords; i++) {
        int s = (sizes[i] * k) / 256;
        if (s < 1) s = 1;
        h += GLYPH_H * s;
      }
      if (h <= bandH) {
        for (int i = 0; i < nWords; i++) {
          int s = (sizes[i] * k) / 256;
          sizes[i] = s < 1 ? 1 : s;
        }
        scaled = 1;
        break;
      }
    }
    if (!scaled) {
      for (int i = 0; i < nWords; i++) sizes[i] = 1;
    }
    sumH = gap * (nWords - 1);
    for (int i = 0; i < nWords; i++) sumH += GLYPH_H * sizes[i];
  }

  int top = bandTop + (bandH - sumH) / 2;
  ox += dx;
  oy += dy;
  for (int i = 0; i < nWords; i++) {
    const int sz = sizes[i];
    const int cell = GLYPH_W * sz;
    const int lineW = cell * lens[i];
    // Letter X is in screen space (before ox) so hue matches panel left→right
    // even when the outline pass draws with a dx offset.
    int x = ox + (SCREEN_W - lineW) / 2;
    const int screenX0 = (SCREEN_W - lineW) / 2;
    g->setTextSize((uint8_t)sz);
    for (int c = 0; c < lens[i]; c++) {
      const uint16_t col =
          perLetter ? nameFillColor(screenX0 + c * cell, cell) : color;
      g->setTextColor(col);
      g->setCursor(x, oy + top);
      g->write((uint8_t)words[i][c]);
      x += cell;
    }
    top += GLYPH_H * sz + gap;
  }
}

// Opaque outline so the name stays readable on top of the stereo wireframes.
static void drawNameOutlined(Arduino_GFX *g, int ox, int oy, const char *name,
                             int bandTop, int bandH, int dx, int dy) {
  static const int8_t off[8][2] = {{-2, -2}, {-2, 0}, {-2, 2}, {0, -2},
                                   {0, 2},   {2, -2}, {2, 0},  {2, 2}};
  for (int i = 0; i < 8; i++) {
    drawNameFitted(g, ox, oy, name, bandTop, bandH, C_BG, dx + off[i][0],
                   dy + off[i][1], false);
  }
  drawNameFitted(g, ox, oy, name, bandTop, bandH, C_ACCENT, dx, dy, true);
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

// === Loop timing ===
// lentAngle is a SAWTOOTH: it ramps 0..TWO_PI and snaps back. Any motion driven
// off it therefore only animates smoothly across the wrap if it completes a
// whole number of cycles within one ramp - otherwise it jumps, every ramp, at
// the same instant on every badge.
//
// That is why the angle now spans the whole loop rather than one revolution,
// and why every rate below is an INTEGER count of cycles per loop. Adding a
// motion at a fractional rate reintroduces the jump, so keep them integers.
#define LENT_PERIOD_MS 6000  // one yaw revolution - the speed things are tuned at
#define LENT_REVS 10         // yaw revolutions per loop
#define LENT_LOOP_MS (LENT_PERIOD_MS * LENT_REVS)

// Cycles per loop. Dividing any of these by LENT_REVS recovers the old rate
// relative to yaw: yaw 1.0, pitch 0.6, ring counter-rotation 0.8, text 0.7.
// Those ratios are unchanged - only the wrap point moved.
#define LENT_YAW LENT_REVS   // 10 - still one revolution per 6s
#define LENT_PITCH 6         // 0.6x yaw, the off-rate tumble
#define LENT_RING_CCW 8      // 0.8x yaw, odd rings turning the other way
#define LENT_TEXT_DEPTH 7    // 0.7x yaw, the sway in and out of the glass

// The tunnel is phase-driven rather than rate-driven (see sceneTunnel), so its
// two motions are counted directly. TUNNEL_PULL was 4.94 recycles per loop at
// the old rate and TUNNEL_TWIST 2.5 turns; both are rounded to integers, which
// is the whole point.
#define TUNNEL_PULL 5
#define TUNNEL_TWIST 2

// Left and right eye colours. Through a correctly aligned lens each eye sees a
// single solid colour, which makes this double as an alignment check: if the
// pitch is off you see magenta blend or shimmer instead. Set both to the same
// colour for a straight lenticular render with no rivalry.
#define C_EYE_L 0x07FF  // cyan
#define C_EYE_R 0xFD20  // amber

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

// === Stereo scenes ===
// Every scene is drawn as a real stereo pair: the same geometry projected from
// two eye positions and interleaved by column parity, so the lens feeds each
// eye its own view. A still image under a lenticular lens just looks like a
// slightly blurry still image - the lens only earns its place when the two eyes
// disagree, which means everything here has to move.
//
// Held in a global and advanced once per frame in modesTick(). This must NOT be
// derived from millis() inside a draw call: draws run once per strip, and
// millis() moves between strips, which would shear the scene across the bands.
// (Definition is near the top of the file so nametag colour can use it.)

#define SCENE_CUBE 0
#define SCENE_PYRAMID 1
#define SCENE_TUNNEL 2
#define SCENE_RINGS 3
#define SCENE_TEXT 4
#define SCENE_MATRIX 5
#define SCENE_DVD 6
#define SCENE_STARS 7

static const char *const SCENE_NAMES[SCENE_COUNT] = {
    "cube",         "pyramid", "tunnel",           "rings",
    "pop-out text", "matrix waterfall", "dvd logo", "starfield"};

const char *sceneName(uint8_t i) { return SCENE_NAMES[i % SCENE_COUNT]; }

uint8_t imageCount = 0;
static char imagePath[IMAGE_MAX][32];
static char imageLabel[IMAGE_MAX][24];

const char *imageName(uint8_t i) {
  return imageCount ? imageLabel[i % imageCount] : "";
}

// One flat index across both kinds: scenes first, then bitmaps.
const char *showName(uint8_t i) {
  i %= SHOW_COUNT;
  return i < SCENE_COUNT ? sceneName(i) : imageName(i - SCENE_COUNT);
}

void imagesBegin() {
  imageCount = 0;
  // false = do not format on failure. An unformatted or absent partition
  // should leave the badge running the procedural scenes, not silently wipe
  // a volume that might just have failed to mount this once.
  if (!LittleFS.begin(false, "/littlefs", 8, "storage")) {
    Serial.println("images: no storage partition (procedural scenes only)");
    return;
  }

  File dir = LittleFS.open("/");
  if (!dir || !dir.isDirectory()) return;

  for (File f = dir.openNextFile(); f && imageCount < IMAGE_MAX;
       f = dir.openNextFile()) {
    const char *n = f.name();
    const size_t len = strlen(n);
    // Exactly one frame, or it is not one of ours.
    if (len > 4 && strcmp(n + len - 4, ".raw") == 0 &&
        f.size() == (size_t)SCREEN_W * SCREEN_H * 2) {
      snprintf(imagePath[imageCount], sizeof(imagePath[0]), "/%s", n);
      snprintf(imageLabel[imageCount], sizeof(imageLabel[0]), "%.*s",
               (int)(len - 4), n);
      imageCount++;
    }
    f.close();
  }
  // Sort by filename. LittleFS enumeration order is NOT specified and differs
  // between badges even with identical files, so without this the slideshow
  // index maps to a different picture on each badge - they change at the same
  // instant but show different art, which looks exactly like a sync bug and
  // is not one. Deterministic ordering is a requirement for content to match,
  // not a nicety.
  for (uint8_t i = 1; i < imageCount; i++) {
    for (uint8_t j = i; j > 0 && strcmp(imagePath[j - 1], imagePath[j]) > 0;
         j--) {
      char tp[sizeof(imagePath[0])], tl[sizeof(imageLabel[0])];
      memcpy(tp, imagePath[j - 1], sizeof(tp));
      memcpy(imagePath[j - 1], imagePath[j], sizeof(tp));
      memcpy(imagePath[j], tp, sizeof(tp));
      memcpy(tl, imageLabel[j - 1], sizeof(tl));
      memcpy(imageLabel[j - 1], imageLabel[j], sizeof(tl));
      memcpy(imageLabel[j], tl, sizeof(tl));
    }
  }
  for (uint8_t i = 0; i < imageCount; i++) {
    Serial.printf("images: [%u] %s\n", (unsigned)(SCENE_COUNT + i),
                  imagePath[i]);
  }

  Serial.printf("images: %u found on storage (%u KB used of %u KB)\n",
                (unsigned)imageCount,
                (unsigned)(LittleFS.usedBytes() / 1024),
                (unsigned)(LittleFS.totalBytes() / 1024));
}

// Streams a bitmap from flash to the panel a band at a time. 150KB will not
// fit in RAM alongside WiFi, and it does not need to: each band is blitted as
// soon as it is read. Bands are small enough that the buffer is a cheap stack
// allocation and large enough that the per-blit overhead disappears.
#define IMG_BAND 16
static void drawImage(Arduino_GFX *g, int ox, int oy, uint8_t idx) {
  if (!imageCount) return;

  // Read ONLY the rows this canvas can actually show. drawScene() runs once
  // per strip during compositing, so reading the whole 150KB file each time
  // meant ~600KB of flash reads per frame with three quarters of it clipped
  // away - enough to make image slides land visibly late while the procedural
  // scenes stayed instant, which reads as badges falling out of sync.
  const int h = g->height();
  int y0 = -oy;
  int y1 = h - oy;
  if (y0 < 0) y0 = 0;
  if (y1 > SCREEN_H) y1 = SCREEN_H;
  if (y0 >= y1) return;  // this strip does not overlap the image at all

  File f = LittleFS.open(imagePath[idx % imageCount], "r");
  if (!f) {
    printCentered(g, "image unreadable", ox, oy + SCREEN_H / 2, 1, C_WARN);
    return;
  }
  f.seek((size_t)y0 * SCREEN_W * 2);

  static uint16_t band[SCREEN_W * IMG_BAND];
  for (int y = y0; y < y1; y += IMG_BAND) {
    const int rows = (y + IMG_BAND <= y1) ? IMG_BAND : (y1 - y);
    const size_t want = (size_t)rows * SCREEN_W * 2;
    if (f.read((uint8_t *)band, want) != want) break;
    g->draw16bitRGBBitmap(ox, oy + y, band, SCREEN_W, rows);
  }
  f.close();
}

// Projects one world point for one eye. Same convergence rule as the cube:
// geometry at CUBE_DIST lands on the glass with zero parallax, nearer pops out,
// further recedes.
static void projectPoint(float x, float y, float z, float eyeX, int cx, int cy,
                         int &px, int &py) {
  if (z < 0.5f) z = 0.5f;
  const float f = CUBE_FOCAL / z;
  const float fConv = CUBE_FOCAL / CUBE_DIST;
  int ax = cx + (int)((x - eyeX) * f + eyeX * fConv);
  int ay = cy + (int)(y * f);
  px = ax < -SCREEN_W ? -SCREEN_W : (ax > 2 * SCREEN_W ? 2 * SCREEN_W : ax);
  py = ay < -SCREEN_H ? -SCREEN_H : (ay > 2 * SCREEN_H ? 2 * SCREEN_H : ay);
}

// Draws one 3D segment as a stereo pair. Everything below builds scenes out of
// this one call, which is what keeps them all correctly stereoscopic.
static void seg3D(Arduino_GFX *g, float x0, float y0, float z0, float x1,
                  float y1, float z1, int cx, int cy) {
  int lx0, ly0, lx1, ly1, rx0, ry0, rx1, ry1;
  projectPoint(x0, y0, z0, -EYE_SEP / 2, cx, cy, lx0, ly0);
  projectPoint(x1, y1, z1, -EYE_SEP / 2, cx, cy, lx1, ly1);
  projectPoint(x0, y0, z0, +EYE_SEP / 2, cx, cy, rx0, ry0);
  projectPoint(x1, y1, z1, +EYE_SEP / 2, cx, cy, rx1, ry1);
  drawLineParity(g, lx0, ly0, lx1, ly1, C_EYE_L, 0);
  drawLineParity(g, rx0, ry0, rx1, ry1, C_EYE_R, 1);
}

// Yaw+pitch, matching the cube's tumble so scenes feel like one family.
//
// Pitch runs at 0.6x yaw so the tumble does not repeat every revolution. It
// still has to close by the end of the LOOP though: at 0.6 cycles per ramp the
// pitch was three fifths of the way round when the angle snapped, so the cube
// and the pyramid both hitched every 6 seconds. Six pitch cycles per ten yaw
// cycles is the same 0.6 ratio and lands both back at their start together.
static void spin(float a, float x, float y, float z, float &ox_, float &oy_,
                 float &oz_) {
  const float sy = sinf(a * LENT_YAW), cy_ = cosf(a * LENT_YAW);
  const float sp = sinf(a * LENT_PITCH), cp = cosf(a * LENT_PITCH);
  const float x1 = x * cy_ + z * sy;
  const float z1 = -x * sy + z * cy_;
  ox_ = x1;
  oy_ = y * cp - z1 * sp;
  oz_ = y * sp + z1 * cp + CUBE_DIST;
}

static void sceneCube(Arduino_GFX *g, float a, int cx, int cy) {
  float p[8][3];
  for (int i = 0; i < 8; i++) {
    spin(a, CUBE_VERTS[i][0], CUBE_VERTS[i][1], CUBE_VERTS[i][2], p[i][0],
         p[i][1], p[i][2]);
  }
  for (int e = 0; e < 12; e++) {
    const uint8_t i = CUBE_EDGES[e][0], j = CUBE_EDGES[e][1];
    seg3D(g, p[i][0], p[i][1], p[i][2], p[j][0], p[j][1], p[j][2], cx, cy);
  }
}

static void scenePyramid(Arduino_GFX *g, float a, int cx, int cy) {
  static const float V[5][3] = {{0, -1.3f, 0},   {-1, 0.7f, -1}, {1, 0.7f, -1},
                                {1, 0.7f, 1},    {-1, 0.7f, 1}};
  static const uint8_t E[8][2] = {{0, 1}, {0, 2}, {0, 3}, {0, 4},
                                  {1, 2}, {2, 3}, {3, 4}, {4, 1}};
  float p[5][3];
  for (int i = 0; i < 5; i++) {
    spin(a, V[i][0], V[i][1], V[i][2], p[i][0], p[i][1], p[i][2]);
  }
  for (int e = 0; e < 8; e++) {
    const uint8_t i = E[e][0], j = E[e][1];
    seg3D(g, p[i][0], p[i][1], p[i][2], p[j][0], p[j][1], p[j][2], cx, cy);
  }
}

// Squares receding to a vanishing point, sliding toward the viewer and
// recycling. This is the strongest depth cue per unit of geometry, because the
// disparity range is enormous - the near ring is nearly in your lap.
static void sceneTunnel(Arduino_GFX *g, float a, int cx, int cy) {
  const int RINGS = 7;
  const float span = 7.0f;
  // Driven off the normalised loop phase rather than off the angle directly.
  // fmodf over `span` used to leave the ring stack part-way between slots when
  // the angle wrapped - the rings were mid-slide and every one of them lurched
  // at once, which read as the whole tunnel stuttering. Counting recycles per
  // loop instead means the stack is back exactly where it started.
  const float u = a * (1.0f / TWO_PI);  // 0..1 across the loop
  for (int i = 0; i < RINGS; i++) {
    // Each ring sits one slot further back and slides toward the viewer,
    // recycling to the rear TUNNEL_PULL times per loop. Adding TUNNEL_PULL
    // keeps the operand positive, since fmodf of a negative stays negative.
    const float slot =
        fmodf((float)i / RINGS - u * TUNNEL_PULL + (float)TUNNEL_PULL, 1.0f);
    const float z = slot * span + 1.2f;
    const float s = 1.6f;
    // Gentle twist adds parallax; TUNNEL_TWIST whole turns per loop.
    const float tw = u * (TWO_PI * TUNNEL_TWIST) + z * 0.15f;
    const float c = cosf(tw), sn = sinf(tw);
    float k[4][2];
    for (int v = 0; v < 4; v++) {
      const float bx = (v == 0 || v == 3) ? -s : s;
      const float by = (v < 2) ? -s : s;
      k[v][0] = bx * c - by * sn;
      k[v][1] = bx * sn + by * c;
    }
    for (int v = 0; v < 4; v++) {
      const int w = (v + 1) % 4;
      seg3D(g, k[v][0], k[v][1], z, k[w][0], k[w][1], z, cx, cy);
    }
  }
}

// Rings parked at fixed depths, each rotating on its own axis. Static in Z on
// purpose: it reads as a stack of objects hanging in space at different
// distances, which is the clearest demonstration that the lens is working.
static void sceneRings(Arduino_GFX *g, float a, int cx, int cy) {
  const int RINGS = 3, SEGS = 14;
  for (int r = 0; r < RINGS; r++) {
    const float z = CUBE_DIST - 1.3f + r * 1.3f;
    const float rad = 0.75f + r * 0.28f;
    // Odd rings counter-rotate at 0.8x. Whole cycles per loop either way, so
    // the alternating rings close together - previously the counter-rotating
    // one was the only thing in the scene that jumped, which looked like that
    // single ring glitching rather than a timing problem.
    const float tilt = a * (r % 2 ? -LENT_RING_CCW : LENT_YAW) + r;
    for (int s = 0; s < SEGS; s++) {
      const float t0 = (float)s * TWO_PI / SEGS;
      const float t1 = (float)(s + 1) * TWO_PI / SEGS;
      // Ring in the XY plane, tipped about X so it presents an ellipse.
      const float y0 = sinf(t0) * rad * cosf(tilt);
      const float y1 = sinf(t1) * rad * cosf(tilt);
      const float dz0 = sinf(t0) * rad * sinf(tilt);
      const float dz1 = sinf(t1) * rad * sinf(tilt);
      seg3D(g, cosf(t0) * rad, y0, z + dz0, cosf(t1) * rad, y1, z + dz1, cx,
            cy);
    }
  }
}

// "DC34" as vector strokes, standing off the background plane and swaying.
// Text is what people try first, and flat text through a lens looks broken -
// so it gets real depth.
static void sceneText(Arduino_GFX *g, float a, int cx, int cy) {
  // Each glyph is a short list of strokes in a 2-wide, 3-tall cell.
  static const int8_t D[][4] = {{-1, -1, 0, -1}, {0, -1, 1, 0},
                                {1, 0, 1, 1},    {1, 1, 0, 2},
                                {0, 2, -1, 2},   {-1, -1, -1, 2}};
  static const int8_t C[][4] = {{1, -1, -1, -1}, {-1, -1, -1, 2}, {-1, 2, 1, 2}};
  static const int8_t T3[][4] = {{-1, -1, 1, -1}, {1, -1, 0, 0},
                                 {0, 0, 1, 1},    {1, 1, -1, 2}};
  static const int8_t F4[][4] = {{1, 2, 1, -1}, {1, -1, -1, 1}, {-1, 1, 1, 1}};

  struct Glyph {
    const int8_t (*st)[4];
    int n;
  };
  const Glyph gl[4] = {{D, 6}, {C, 3}, {T3, 4}, {F4, 3}};

  // Sway is already whole-cycle. Depth was the broken one: at 0.7 cycles per
  // ramp the letters were caught mid-travel when the angle wrapped, so they
  // snapped back through the glass instead of easing out of it.
  const float sway = sinf(a * LENT_YAW) * 0.5f;
  const float depth = CUBE_DIST - 1.6f + cosf(a * LENT_TEXT_DEPTH) * 0.35f;
  const float sc = 0.42f;

  for (int gi = 0; gi < 4; gi++) {
    const float gx = (gi - 1.5f) * 1.25f;
    for (int s = 0; s < gl[gi].n; s++) {
      const int8_t *v = gl[gi].st[s];
      // Sway rotates about Y, so the letters swing through depth rather than
      // just sliding sideways - that is what makes them pop.
      const float x0 = gx + v[0] * sc, x1 = gx + v[2] * sc;
      const float c = cosf(sway), sn = sinf(sway);
      seg3D(g, x0 * c, v[1] * sc, depth + x0 * sn, x1 * c, v[3] * sc,
            depth + x1 * sn, cx, cy);
    }
  }
}

static void sceneMatrix(Arduino_GFX *g, float a, int ox, int oy) {
  static const char GLYPHS[] = "01ABCDEFGHIJKLMNOPQRSTUVWXYZ<>*+:;=-";
  static const int NGLYPHS = (int)sizeof(GLYPHS) - 1;

  const int cellW = GLYPH_W;  // 6
  const int cellH = GLYPH_H;  // 8
  const int cols = SCREEN_W / cellW;
  const int rows = SCREEN_H / cellH;
  const int period = rows + 14;  // head travels off-screen before wrapping

  // Bright → mid → dark green trail (RGB565).
  const uint16_t C_HEAD = 0xCFF9;  // near-white green
  const uint16_t C_NEAR = 0x07E0;  // full green
  const uint16_t C_MID = 0x04A0;
  const uint16_t C_FAR = 0x0220;

  g->setTextSize(1);

  const float turns = a / TWO_PI;  // 0..1 per loop sawtooth
  for (int c = 0; c < cols; c++) {
    const uint8_t seed = (uint8_t)(c * 37u + 11u);
    // 2..5 full fall-throughs per loop, staggered per column.
    const int falls = 2 + (int)(seed % 4);
    const float headF =
        fmodf(turns * (float)(LENT_MATRIX * falls) * (float)period + seed,
              (float)period);
    const int head = (int)headF;
    const int trail = 7 + (int)(seed % 5);

    for (int t = 0; t < trail; t++) {
      const int row = head - t;
      if (row < 0 || row >= rows) continue;
      const char ch = GLYPHS[(seed + row * 7 + c * 3) % NGLYPHS];
      uint16_t color = C_FAR;
      if (t == 0) color = C_HEAD;
      else if (t == 1) color = C_NEAR;
      else if (t < 4) color = C_MID;
      g->setTextColor(color);
      g->setCursor(ox + c * cellW, oy + row * cellH);
      g->write((uint8_t)ch);
    }
  }
}

// Ping-pong 0→1→0 so position is a pure function of lentAngle (strip-safe).
static float pingpong01(float u) {
  u = fmodf(u, 2.0f);
  if (u < 0.0f) u += 2.0f;
  return (u < 1.0f) ? u : (2.0f - u);
}

// Classic DVD mark: big "DVD" over a solid oval (no "VIDEO" text).
#define DVD_LOGO_W 78
#define DVD_LOGO_H 44
static void drawDvdLogo(Arduino_GFX *g, int x0, int y0, uint16_t col) {
  g->setTextSize(3);
  g->setTextColor(col);
  const char *dvd = "DVD";
  const int dtw = textWidth(dvd, 3);
  g->setCursor(x0 + (DVD_LOGO_W - dtw) / 2, y0 + 2);
  g->print(dvd);

  const int ow = 72;
  const int oh = 16;
  const int ecx = x0 + DVD_LOGO_W / 2;
  const int ecy = y0 + DVD_LOGO_H - oh / 2 - 1;
  g->fillEllipse(ecx, ecy, ow / 2, oh / 2, col);
}

// DVD-logo bounce. Flat scene; colour advances on every wall hit.
static void sceneDvdLogo(Arduino_GFX *g, float a, int ox, int oy) {
  static const uint16_t DVD_COLS[] = {
      0xF800,  // red
      0x07E0,  // green
      0x001F,  // blue
      0xFFE0,  // yellow
      0x07FF,  // cyan
      0xF81F,  // magenta
      0xFD20,  // orange
      0xFFFF,  // white
  };
  const int nCol = (int)(sizeof(DVD_COLS) / sizeof(DVD_COLS[0]));
  const int maxX = SCREEN_W - DVD_LOGO_W;
  const int maxY = SCREEN_H - DVD_LOGO_H;
  if (maxX < 0 || maxY < 0) return;
  const float t = a / TWO_PI;
  // Incommensurate rates ≈ classic DVD path (corner hits are rare).
  const float ux = t * 13.0f;
  const float uy = t * 9.0f;
  const int x = ox + (int)(pingpong01(ux) * (float)maxX);
  const int y = oy + (int)(pingpong01(uy) * (float)maxY);
  // Each integer step of ux/uy is an edge bounce; corners bump twice (fine).
  const int bounces = (int)floorf(ux) + (int)floorf(uy);
  const uint16_t col = DVD_COLS[((bounces % nCol) + nCol) % nCol];
  drawDvdLogo(g, x, y, col);
}

// Classic Windows-style starfield: stars rush toward the viewer.
static void sceneStars(Arduino_GFX *g, float a, int ox, int oy) {
  const int N = 80;
  const float u = a / TWO_PI;  // 0..1 across the lenticular loop
  const int cx = ox + SCREEN_W / 2;
  const int cy = oy + SCREEN_H / 2;

  for (int i = 0; i < N; i++) {
    // Stable pseudo-random lane per star (no heap, no millis in draw).
    const uint32_t s = (uint32_t)(i * 2654435761u + 1013904223u);
    const float x0 = ((s & 0xFFFF) / 32768.0f) - 1.0f;
    const float y0 = (((s >> 16) & 0xFFFF) / 32768.0f) - 1.0f;
    // Depth cycles toward the camera; LENT_STARS packs many rushes per loop.
    float z =
        fmodf((1.0f - u) * 7.0f * (float)LENT_STARS + (i * 0.41f), 7.0f) +
        0.4f;
    const float inv = 1.0f / z;
    const int px = cx + (int)(x0 * 130.0f * inv);
    const int py = cy + (int)(y0 * 170.0f * inv);
    if (px < ox || px >= ox + SCREEN_W || py < oy || py >= oy + SCREEN_H) {
      continue;
    }
    // Nearer = brighter + slightly larger; longer streak at speed.
    uint16_t col = 0x4A69;  // dim
    int sz = 1;
    if (z < 1.4f) {
      col = C_FG;
      sz = 2;
    } else if (z < 3.0f) {
      col = C_DIM;
      sz = 1;
    }
    g->fillRect(px, py, sz, sz, col);
    if (z < 2.5f) {
      const float tail = (z < 1.2f) ? 0.78f : 0.88f;
      const int tx = cx + (int)(x0 * 130.0f * inv * tail);
      const int ty = cy + (int)(y0 * 170.0f * inv * tail);
      g->drawLine(px, py, tx, ty, col);
    }
  }
}

// Procedural scene geometry only — no caption / alignment chrome. Shared by
// the SHOW modes and the nametag background rotator.
static void drawProceduralSceneBg(Arduino_GFX *g, int ox, int oy, uint8_t idx) {
  g->fillRect(ox, oy, SCREEN_W, SCREEN_H, C_BG);

  const int cx = ox + SCREEN_W / 2;
  const int cy = oy + SCREEN_H / 2 - 10;

  switch (idx % SCENE_COUNT) {
    case SCENE_PYRAMID: scenePyramid(g, lentAngle, cx, cy); break;
    case SCENE_TUNNEL: sceneTunnel(g, lentAngle, cx, cy); break;
    case SCENE_RINGS: sceneRings(g, lentAngle, cx, cy); break;
    case SCENE_TEXT: sceneText(g, lentAngle, cx, cy); break;
    case SCENE_MATRIX: sceneMatrix(g, lentAngle, ox, oy); break;
    case SCENE_DVD: sceneDvdLogo(g, lentAngle, ox, oy); break;
    case SCENE_STARS: sceneStars(g, lentAngle, ox, oy); break;
    case SCENE_CUBE:
    default: sceneCube(g, lentAngle, cx, cy); break;
  }
}

// Draws scene `idx` at the current angle. Used by both slideshow and static.
void drawScene(Arduino_GFX *g, int ox, int oy, uint8_t idx) {
  idx %= SHOW_COUNT;

  // Bitmaps take the whole panel and carry their own caption-free framing.
  if (idx >= SCENE_COUNT) {
    drawImage(g, ox, oy, idx - SCENE_COUNT);
    g->fillRect(ox, oy + SCREEN_H - 20, SCREEN_W, 20, C_BG);
    printCentered(g, showName(idx), ox, oy + SCREEN_H - 14, 1, C_DIM);
    return;
  }

  drawProceduralSceneBg(g, ox, oy, idx);

  // Pitch-alignment target: a 1px interlace of the same two colours the scenes
  // use. Through a correctly aligned lens each eye sees one solid colour.
  // Skip it for flat (non-stereo) scenes — otherwise it reads as a bright
  // stripe above the caption / battery.
  if (idx != SCENE_MATRIX && idx != SCENE_STARS && idx != SCENE_DVD) {
    const int stripY = oy + SCREEN_H - 36;
    for (int x = 0; x < SCREEN_W; x++) {
      g->drawFastVLine(ox + x, stripY, 14, (x & 1) ? C_EYE_R : C_EYE_L);
    }
  }

  g->fillRect(ox, oy + SCREEN_H - 20, SCREEN_W, 20, C_BG);
  printCentered(g, showName(idx), ox, oy + SCREEN_H - 14, 1, C_DIM);
}

// Nametag: name on top of a background animation. Background either rotates
// through procedural scenes, or stays locked on whichever show was pinned by
// long-pressing the unlocked nametag rotator.
void drawNametag(Arduino_GFX *g, int ox, int oy) {
  // ±2 outline; keep the layout band inside the header/footer so the outline
  // does not draw under those bars.
  const int headerH = 36;
  const int footerH = 24;
  const int outMax = 2;
  const int bandTop = headerH + outMax;
  const int bandH = SCREEN_H - headerH - footerH - 2 * outMax;

  const uint8_t bg = (SHOW_COUNT > 0) ? (uint8_t)(nameBgShow % SHOW_COUNT) : 0;
  if (bg >= SCENE_COUNT) {
    drawImage(g, ox, oy, bg - SCENE_COUNT);
  } else {
    drawProceduralSceneBg(g, ox, oy, bg);
  }

  g->fillRect(ox, oy, SCREEN_W, headerH, C_BG);
  g->fillRect(ox, oy + SCREEN_H - footerH, SCREEN_W, footerH, C_BG);

  printCentered(g, "DEF CON 34", ox, oy + 14, 1, C_DIM);
  drawNameOutlined(g, ox, oy, nametagName, bandTop, bandH, 0, 0);
  printCentered(g, screenIsLocked() ? "long press to unlock" : "touch to exit",
                ox, oy + SCREEN_H - 16, 1, C_DIM);
}


void drawModeScreen(Arduino_GFX *g, int ox, int oy) {
  switch (activeMode()) {
    case MODE_NAMETAG:
      drawNametag(g, ox, oy);
      return;
    case MODE_SLIDESHOW:
      drawScene(g, ox, oy, slideIndex);
      return;
    case MODE_WIFI_SCAN: {
      g->fillRect(ox, oy, SCREEN_W, SCREEN_H, C_BG);
      printCentered(g, "WIFI SCANNER", ox, oy + 18, 2, C_ACCENT);
      g->drawFastHLine(ox + 12, oy + 44, SCREEN_W - 24, C_DIM);
      const uint8_t count = wifiScanCount();
      if (wifiScanRunning()) {
        const char *dots[] = {"scanning", "scanning.", "scanning..", "scanning..."};
        printCentered(g, dots[(millis() / 250) & 3], ox, oy + 58, 1, C_NAV);
      } else if (!count) {
        printCentered(g, "no networks seen", ox, oy + 88, 1, C_DIM);
      }
      if (scanOffset > scanMaxOffset()) scanOffset = scanMaxOffset();
      const uint8_t remaining = count > scanOffset ? count - scanOffset : 0;
      const uint8_t visible =
          remaining < WIFI_SCAN_VISIBLE ? remaining : WIFI_SCAN_VISIBLE;
      for (uint8_t i = 0; i < visible; i++) {
        const uint8_t index = scanOffset + i;
        const int y = oy + 58 + i * 42;
        char line[48];
        snprintf(line, sizeof(line), "%c %-22.22s %4d",
                 wifiScanSecure(index) ? '*' : ' ', wifiScanSsid(index),
                 (int)wifiScanRssi(index));
        g->setTextSize(1);
        g->setTextColor(index == 0 ? C_ACCENT : C_FG);
        g->setCursor(ox + 8, y);
        g->print(line);

        snprintf(line, sizeof(line), "ch%-2u %-5s %s",
                 (unsigned)wifiScanChannel(index), wifiScanAuth(index),
                 wifiScanBssid(index));
        g->setTextColor(C_DIM);
        g->setCursor(ox + 8, y + 12);
        g->print(line);

        int strength = ((int)wifiScanRssi(index) + 100) * 2;
        if (strength < 0) strength = 0;
        if (strength > 100) strength = 100;
        g->drawRect(ox + 8, y + 25, SCREEN_W - 16, 5, C_DIM);
        g->fillRect(ox + 9, y + 26,
                    (SCREEN_W - 18) * strength / 100, 3,
                    strength >= 70 ? C_OK : (strength >= 35 ? C_NAV : C_WARN));
      }
      char summary[32];
      if (count) {
        snprintf(summary, sizeof(summary), "%u-%u/%u  count this session %u",
                 (unsigned)scanOffset + 1,
                 (unsigned)(scanOffset + visible), (unsigned)count,
                 (unsigned)wifiScanSessionCount());
      } else {
        snprintf(summary, sizeof(summary), "0 now  session %u",
                 (unsigned)wifiScanSessionCount());
      }
      printCentered(g, summary, ox, oy + SCREEN_H - 46, 1, C_DIM);
      printCentered(g, shareWifiScans ? "SHARING TO FLEET" : "PRIVATE / LOCAL",
            ox, oy + SCREEN_H - 32, 1,
            shareWifiScans ? C_OK : C_NAV);
      printCentered(g,
                    screenIsLocked() ? "long press to unlock"
                                     : "up/down scroll  tap exits",
                    ox, oy + SCREEN_H - 18, 1, C_DIM);
      return;
    }
    case MODE_BLE_SCAN: {
      g->fillRect(ox, oy, SCREEN_W, SCREEN_H, C_BG);
      printCentered(g, "BADGE SCANNER", ox, oy + 10, 2, C_ACCENT);

      const uint8_t nearby = bleNearbyCount();
      char headline[32];
      snprintf(headline, sizeof(headline), "%u nearby now", (unsigned)nearby);
      printCentered(g, headline, ox, oy + 38, 1,
                    nearby ? C_OK : C_DIM);
      g->drawFastHLine(ox + 12, oy + 52, SCREEN_W - 24, C_DIM);

      const uint8_t total = blePeerCount();
      const uint8_t maxOff = total > BLE_SCAN_VISIBLE
                                 ? total - BLE_SCAN_VISIBLE : 0;
      if (bleOffset > maxOff) bleOffset = maxOff;
      const uint8_t remaining = total > bleOffset ? total - bleOffset : 0;
      const uint8_t visible = remaining < BLE_SCAN_VISIBLE
                                  ? remaining : BLE_SCAN_VISIBLE;
      for (uint8_t i = 0; i < visible; i++) {
        const uint8_t idx = bleOffset + i;
        const int y = oy + 58 + i * 34;
        const uint32_t age = blePeerAgeMs(idx);
        const bool isNearby = age <= 30000;
        char line[32];
        snprintf(line, sizeof(line), "%s  %ddBm", blePeerId(idx),
                 (int)blePeerRssi(idx));
        g->setTextSize(1);
        g->setTextColor(isNearby ? C_FG : C_DIM);
        g->setCursor(ox + 8, y);
        g->print(line);

        char ago[16];
        if (age < 1000) snprintf(ago, sizeof(ago), "now");
        else if (age < 60000) snprintf(ago, sizeof(ago), "%us ago",
                                       (unsigned)(age / 1000));
        else snprintf(ago, sizeof(ago), "%um ago",
                      (unsigned)(age / 60000));
        g->setCursor(ox + 8, y + 12);
        g->setTextColor(C_DIM);
        g->print(ago);

        int strength = ((int)blePeerRssi(idx) + 100) * 2;
        if (strength < 0) strength = 0;
        if (strength > 100) strength = 100;
        g->drawRect(ox + 8, y + 25, SCREEN_W - 16, 4, C_DIM);
        g->fillRect(ox + 9, y + 26,
                    (SCREEN_W - 18) * strength / 100, 2,
                    isNearby ? C_OK : C_DIM);
      }

      if (!total) {
        printCentered(g, "no badges seen yet", ox, oy + 100, 1, C_DIM);
        printCentered(g, "BLE is scanning passively", ox, oy + 120, 1, C_DIM);
      }

      char summary[40];
      snprintf(summary, sizeof(summary), "%u seen this session",
               (unsigned)bleSessionCount());
      printCentered(g, summary, ox, oy + SCREEN_H - 32, 1, C_DIM);
      printCentered(g,
                    screenIsLocked() ? "long press to unlock"
                                     : "up/down scroll  tap exits",
                    ox, oy + SCREEN_H - 18, 1, C_DIM);
      return;
    }
    case MODE_GAME_2048:
      drawGame2048(g, ox, oy);
      return;
    default:
      // Any other row is one animation held on its own. It keeps moving - the
      // only thing this turns off is advancing to the next scene. A frozen
      // frame is the one thing that looks bad through a lenticular lens.
      drawScene(g, ox, oy, activeMode() - MODE_SCENE_FIRST);
      return;
  }
}

// Animation phase, in milliseconds, shared across badges whenever they share a
// clock.
//
// Deriving phase from absolute time rather than uptime is what makes two badges
// agree WITHOUT talking to each other - there is no traffic between them, they
// just both know what time it is. With SNTP the agreement is exact to well
// under the 2.5s scene dwell.
//
// Without a clock this falls back to millis(), which is uptime. Two badges
// reset within a few seconds of each other then free-run in near-lockstep and
// drift apart slowly - which is exactly the accidental sync seen on the bench,
// and why it looked like it already worked.
uint32_t syncPhaseMs() {
  struct timeval tv;
  if (gettimeofday(&tv, nullptr) == 0 && tv.tv_sec > 1700000000L) {
    return (uint32_t)(((uint64_t)tv.tv_sec * 1000ULL + tv.tv_usec / 1000ULL) &
                      0xFFFFFFFFULL);
  }
  return millis();
}

bool timeIsSynced() {
  struct timeval tv;
  return gettimeofday(&tv, nullptr) == 0 && tv.tv_sec > 1700000000L;
}

void modesEnter(uint32_t now) {
  lastFrame = 0;  // force a frame on the next tick
  if (activeMode() == MODE_WIFI_SCAN) {
    scanOffset = 0;
    wifiScanModeEnter();
    scanRefreshAt = now + 500;
  }
  if (activeMode() == MODE_BLE_SCAN) {
    bleOffset = 0;
  }
  if (activeMode() == MODE_GAME_2048) {
    game2048Reset();
  }
}

void modesExit() {
  if (activeMode() == MODE_WIFI_SCAN) wifiScanModeExit();
}

bool modesHandleGesture(Gesture gesture) {
  if (activeMode() == MODE_WIFI_SCAN) {
    if (gesture == G_UP) {
      const uint8_t maxOffset = scanMaxOffset();
      if (scanOffset < maxOffset) scanOffset++;
      LOGF("wifi scan: scroll %u/%u\n", (unsigned)scanOffset,
           (unsigned)maxOffset);
      return true;
    }
    if (gesture == G_DOWN) {
      if (scanOffset > 0) scanOffset--;
      LOGF("wifi scan: scroll %u/%u\n", (unsigned)scanOffset,
           (unsigned)scanMaxOffset());
      return true;
    }
    return false;
  }
  if (activeMode() == MODE_BLE_SCAN) {
    const uint8_t total = blePeerCount();
    const uint8_t maxOff = total > BLE_SCAN_VISIBLE
                               ? total - BLE_SCAN_VISIBLE : 0;
    if (gesture == G_UP) {
      if (bleOffset < maxOff) bleOffset++;
      return true;
    }
    if (gesture == G_DOWN) {
      if (bleOffset > 0) bleOffset--;
      return true;
    }
    return false;
  }
  if (activeMode() == MODE_GAME_2048) {
    return game2048HandleGesture(gesture, lastX, lastY);
  }
  return false;
}

bool modesTick(uint32_t now) {
  bool dirty = false;

  if (activeMode() == MODE_WIFI_SCAN) {
    if (wifiScanTick()) {
      mqttPublishWifiScan();
      scanRefreshAt = now + WIFI_SCAN_REFRESH_MS;
      dirty = true;
    }
    if (!wifiScanRunning() && !mqttIsConnecting() &&
      (int32_t)(now - scanRefreshAt) >= 0) {
      wifiScanStart();
      scanRefreshAt = now + WIFI_SCAN_REFRESH_MS;
      dirty = true;
    }
    if (wifiScanRunning() && now - lastFrame >= 250) {
      lastFrame = now;
      dirty = true;
    }
    return dirty;
  }

  if (activeMode() == MODE_BLE_SCAN) {
    if (now - lastFrame >= 1000) {
      lastFrame = now;
      dirty = true;
    }
    return dirty;
  }

  const uint32_t phase = syncPhaseMs();

  // Slideshow index comes from the shared phase, not from a local counter, so
  // every badge on the same clock is showing the same scene at the same
  // instant. A counter would drift the moment one badge rebooted.
  if (activeMode() == MODE_SLIDESHOW) {
    const uint8_t idx = (uint8_t)((phase / SLIDE_MS) % SHOW_COUNT);
    if (idx != slideIndex) {
      slideIndex = idx;
      dirty = true;
      // Logged with the clock source: two badges on a wall clock must land on
      // the same index at the same instant, with no traffic between them.
      LOGF("slide -> %u (%s) phase=%lu\n", (unsigned)idx,
           timeIsSynced() ? "ntp" : "uptime", (unsigned long)phase);
    }
  }


  // Nametag backgrounds rotate unless a long-press pinned one.
  // Procedural scenes only while unlocked, so a missing LittleFS image never
  // blanks the tag.
  if (activeMode() == MODE_NAMETAG && !nameBgLocked) {
    const uint8_t idx = (uint8_t)((phase / NAME_BG_MS) % SCENE_COUNT);
    if (idx != nameBgShow) {
      nameBgShow = idx;
      dirty = true;
      LOGF("nametag bg -> %s\n", sceneName(idx));
    }
  } else if (activeMode() == MODE_NAMETAG && nameBgLocked && SHOW_COUNT > 0) {
    const uint8_t clamped = (uint8_t)(nameBgShow % SHOW_COUNT);
    if (clamped != nameBgShow) {
      nameBgShow = clamped;
      dirty = true;
    }
  }

  // Every mode here animates, so all of them need frames.
  if (now - lastFrame >= LENT_FRAME_MS) {
    lastFrame = now;
    // Phase, again shared: two badges show the same pose, not just the same
    // scene. The angle is computed once per frame - never inside a draw call,
    // which runs once per strip and would shear the scene across the bands.
    // Ramps 0..TWO_PI once per LOOP, not once per revolution. Scenes multiply
    // it by their own whole-cycle counts, so every motion in every scene is
    // back at its starting pose at the instant this wraps.
    lentAngle = (float)(phase % LENT_LOOP_MS) * (TWO_PI / LENT_LOOP_MS);
    dirty = true;
  }
  return dirty;
}

bool modesPinCurrentAsNametag() {
  if (SHOW_COUNT == 0 || activeMode() != MODE_NAMETAG) return false;
  // Pin whatever is on screen right now (rotating background).
  if (nameBgLocked) return false;  // caller should unpin instead

  nameBgShow = (uint8_t)(nameBgShow % SHOW_COUNT);
  nameBgLocked = true;
  saveSettings();
  LOGF("nametag pinned -> %s (show %u)\n", showName(nameBgShow),
       (unsigned)nameBgShow);
  return true;
}

bool modesUnpinNametag() {
  if (!nameBgLocked) return false;
  nameBgLocked = false;
  // Drop back into the procedural rotator from a safe index.
  if (nameBgShow >= SCENE_COUNT) nameBgShow = 0;
  saveSettings();
  LOGF("nametag unpinned — backgrounds rotating again\n");
  return true;
}
