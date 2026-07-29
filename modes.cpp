// modes.cpp - see modes.h.

#include "modes.h"

#include "display.h"
#include "menus.h"
#include <sys/time.h>

#include <LittleFS.h>

#include "store.h"

static uint8_t slideIndex = 0;
static uint32_t lastFrame = 0;

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

// Fits the name as large as the panel allows, wrapping at a space when two
// lines let the glyphs be bigger than one. That is usually a big win: "YOUR
// NAME" on one line is capped at size 4 by its 9 characters, but split across
// two lines the longest half is 4 characters and it fits at size 9.
//
// A nametag is meant to be read across a room, so this maximises rather than
// picking a comfortable default.
static void drawNameFitted(Arduino_GFX *g, int ox, int oy, const char *name,
                           int bandTop, int bandH, uint16_t color) {
  const int len = (int)strlen(name);
  if (len == 0) return;

  const int maxW = SCREEN_W - 16;  // keep clear of the accent border
  const int CAP = 10;              // past this the 5x7 font is just blocks

  int one = maxW / (GLYPH_W * len);
  if (one > bandH / GLYPH_H) one = bandH / GLYPH_H;
  if (one > CAP) one = CAP;
  if (one < 1) one = 1;

  // Split at the space nearest the middle, so the two lines are balanced and
  // the longest one - which is what limits the size - is as short as possible.
  int split = -1, bestDist = 9999;
  for (int i = 0; i < len; i++) {
    if (name[i] != ' ') continue;
    const int d = abs(i - len / 2);
    if (d < bestDist) {
      bestDist = d;
      split = i;
    }
  }

  // A long name with no space in it cannot wrap at a word, and would otherwise
  // collapse to a size nobody can read across a room. Break it mid-word
  // instead - ugly, but legible beats correct here.
  bool hardWrap = false;
  if (split < 0 && len > 8) {
    split = len / 2;
    hardWrap = true;
  }

  int two = 0, l1 = 0, l2 = 0;
  if (split > 0 && split < len - 1) {
    l1 = split;
    l2 = len - split - (hardWrap ? 0 : 1);
    const int longest = l1 > l2 ? l1 : l2;
    two = maxW / (GLYPH_W * longest);
    const int hLimit = (bandH - 8) / (2 * GLYPH_H);
    if (two > hLimit) two = hLimit;
    if (two > CAP) two = CAP;
  }

  if (two > one) {
    char a[40], b[40];
    snprintf(a, sizeof(a), "%.*s", l1, name);
    snprintf(b, sizeof(b), "%s", name + split + (hardWrap ? 0 : 1));
    const int block = 2 * GLYPH_H * two + 8;
    const int top = bandTop + (bandH - block) / 2;
    printCentered(g, a, ox, oy + top, (uint8_t)two, color);
    printCentered(g, b, ox, oy + top + GLYPH_H * two + 8, (uint8_t)two, color);
  } else {
    const int top = bandTop + (bandH - GLYPH_H * one) / 2;
    printCentered(g, name, ox, oy + top, (uint8_t)one, color);
  }
}

void drawNametag(Arduino_GFX *g, int ox, int oy) {
  g->fillRect(ox, oy, SCREEN_W, SCREEN_H, C_ACCENT);
  g->fillRect(ox + 6, oy + 6, SCREEN_W - 12, SCREEN_H - 12, C_BG);

  printCentered(g, "DEF CON 34", ox, oy + 26, 1, C_DIM);

  // Everything between the header and the footer belongs to the name.
  drawNameFitted(g, ox, oy, nametagName, 52, 216, C_ACCENT);

  printCentered(g, "touch to exit", ox, oy + SCREEN_H - 26, 1, C_DIM);
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
float lentAngle = 0.0f;

#define SCENE_CUBE 0
#define SCENE_PYRAMID 1
#define SCENE_TUNNEL 2
#define SCENE_RINGS 3
#define SCENE_TEXT 4

static const char *const SCENE_NAMES[SCENE_COUNT] = {
    "cube", "pyramid", "tunnel", "rings", "pop-out text"};

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

  g->fillRect(ox, oy, SCREEN_W, SCREEN_H, C_BG);

  const int cx = ox + SCREEN_W / 2;
  const int cy = oy + SCREEN_H / 2 - 10;

  switch (idx % SCENE_COUNT) {
    case SCENE_PYRAMID: scenePyramid(g, lentAngle, cx, cy); break;
    case SCENE_TUNNEL: sceneTunnel(g, lentAngle, cx, cy); break;
    case SCENE_RINGS: sceneRings(g, lentAngle, cx, cy); break;
    case SCENE_TEXT: sceneText(g, lentAngle, cx, cy); break;
    case SCENE_CUBE:
    default: sceneCube(g, lentAngle, cx, cy); break;
  }

  // Pitch-alignment target: a 1px interlace of the same two colours the scenes
  // use. Through a correctly aligned lens each eye sees one solid colour.
  const int stripY = oy + SCREEN_H - 36;
  for (int x = 0; x < SCREEN_W; x++) {
    g->drawFastVLine(ox + x, stripY, 14, (x & 1) ? C_EYE_R : C_EYE_L);
  }

  g->fillRect(ox, oy + SCREEN_H - 20, SCREEN_W, 20, C_BG);
  printCentered(g, showName(idx), ox, oy + SCREEN_H - 14, 1, C_DIM);
}

void drawModeScreen(Arduino_GFX *g, int ox, int oy) {
  switch (activeMode()) {
    case MODE_NAMETAG:
      drawNametag(g, ox, oy);
      return;
    case MODE_SLIDESHOW:
      drawScene(g, ox, oy, slideIndex);
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
  (void)now;
  lastFrame = 0;  // force a frame on the next tick
}

bool modesTick(uint32_t now) {
  bool dirty = false;
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
