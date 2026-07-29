// input.cpp - see input.h.

#include "input.h"

#include <Wire.h>

int startX = 0, startY = 0, lastX = 0, lastY = 0;

static bool touchDown = false;
static int bestDx = 0, bestDy = 0;  // furthest travel seen during the stroke

static uint8_t readReg(uint8_t reg);
static void writeReg(uint8_t reg, uint8_t val);

void touchBegin() {
  Wire.begin(TP_SDA, TP_SCL, 400000);
  // Without a bus timeout a confused controller can stall the loop for a
  // second per poll, which reads as a lock-up.
  Wire.setTimeOut(8);
  // Longer than the datasheet minimum on purpose. The 20ms/60ms sequence that
  // worked on the first badge leaves this one ACKing its address with every
  // register reading 0x00 - powered and address-decoding, but core not running.
  pinMode(TP_RST, OUTPUT);
  digitalWrite(TP_RST, LOW);
  delay(50);
  digitalWrite(TP_RST, HIGH);
  delay(200);

  // Probe the controller. A badge whose touch flex is unseated looks identical
  // to a firmware bug from the outside, so say which it is at boot rather than
  // leaving it to be guessed at.
  Wire.beginTransmission(TP_ADDR);
  const uint8_t err = Wire.endTransmission();
  Serial.printf("touch: device at 0x%02X %s (i2c err %u)\n", TP_ADDR,
                err == 0 ? "ACKED" : "NOT RESPONDING", (unsigned)err);
  if (err != 0) {
    Serial.println("touch: check the connector 8 flex - no device on the bus");
    return;
  }

  // Identify the part. A bus that ACKs but never reports a finger looks the
  // same whether the panel is unseated, asleep, or simply not the chip we
  // think it is - reading the ID separates those.
  //
  // KNOWN ISSUE: the second badge reports ChipID 0xA3 = 0x64. The CST816
  // family returns 0xB4 (S), 0xB5 (T) or 0xB6 (D). This driver's register map
  // - reg 0x01 as GestureID/FingerNum/XH/XL/YH/YL - is the CST816S map, so on
  // a 0x64 part it reads a valid-looking but meaningless "0 fingers" forever.
  // That is why touch is dead on that unit while the bus scan is clean.
  Serial.printf("touch: chipId=0x%02X fwVer=0x%02X\n", readReg(0xA7),
                readReg(0xA9));

  // CST816S auto-sleeps after a few seconds idle. While asleep it still ACKs
  // I2C and cheerfully reports "0 fingers" forever, which is indistinguishable
  // from nobody touching it. Disabling that is what makes a poll-only driver
  // (no INT line) work reliably.
  writeReg(0xFE, 0x01);  // DisAutoSleep
  Serial.printf("touch: autosleep disabled (readback 0x%02X)\n", readReg(0xFE));
}

// Rolling counts, dumped periodically so a dead bus is visible without anyone
// having to touch the screen.
static uint32_t nFail = 0, nUp = 0, nDown = 0, lastReport = 0;

// A device that ACKs its address but reads back 0x00 from every register is
// ambiguous: it could be a chip that is present but wedged, or it could be SDA
// held low, which makes EVERY address appear to ACK. Scanning the bus tells
// those apart, and it is the difference between a firmware bug and a connector.
static void busScan() {
  int found = 0;
  char list[96] = "";
  for (uint8_t a = 0x08; a < 0x78; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      found++;
      if (strlen(list) < sizeof(list) - 6) {
        snprintf(list + strlen(list), sizeof(list) - strlen(list), "0x%02X ", a);
      }
    }
  }
  Serial.printf("i2c scan: %d device(s) %s\n", found, list);
  if (found > 8) {
    Serial.println(
        "i2c scan: everything ACKs - SDA is stuck low. That is a wiring/flex "
        "fault on connector 8, not firmware.");
  } else if (found == 0) {
    Serial.println("i2c scan: bus empty - touch controller unpowered or held "
                   "in reset.");
  }
}

void touchDiag(uint32_t now) {
  if (now - lastReport < 5000) return;
  const bool first = (lastReport == 0);
  lastReport = now;
  if (first) busScan();
  // 0xA3 is ChipID on the CST816 family (0xB4/B5/B6 depending on variant);
  // 0xA7 is ProjID, 0xA9 FwVersion. All three reading 0x00 means the core is
  // not running, not that we are reading the wrong register.
  LOGF("touch diag: down=%lu up=%lu failed=%lu id(A3)=0x%02X proj(A7)=0x%02X "
       "fw(A9)=0x%02X sleep(FE)=0x%02X\n",
       (unsigned long)nDown, (unsigned long)nUp, (unsigned long)nFail,
       readReg(0xA3), readReg(0xA7), readReg(0xA9), readReg(0xFE));
  nFail = nUp = nDown = 0;
}

static uint8_t readReg(uint8_t reg) {
  Wire.beginTransmission(TP_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0xFF;
  if (Wire.requestFrom((uint8_t)TP_ADDR, (uint8_t)1) != 1) return 0xFF;
  return Wire.read();
}

static void writeReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(TP_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

static TouchState touchRead(int &x, int &y) {
  Wire.beginTransmission(TP_ADDR);
  Wire.write(0x00);  // read from 0x00 so the raw dump covers the whole header
  if (Wire.endTransmission(false) != 0) return T_FAILED;
  if (Wire.requestFrom((uint8_t)TP_ADDR, (uint8_t)7) != 7) return T_FAILED;

  uint8_t raw[7];
  for (int i = 0; i < 7; i++) raw[i] = Wire.read();

  // Raw-byte tap. The CST816S register map may not be this part's map, so log
  // whatever it actually reports the moment ANY header byte goes non-zero.
  // If a finger produces bytes here, the panel works and only the decode is
  // wrong; if it produces nothing, the fault is below the driver entirely.
  static uint32_t lastRawLog = 0;
  bool anySet = false;
  for (int i = 0; i < 7; i++) {
    if (raw[i]) anySet = true;
  }
  if (anySet && millis() - lastRawLog > 120) {
    lastRawLog = millis();
    LOGF("touch RAW %02X %02X %02X %02X %02X %02X %02X\n", raw[0], raw[1],
         raw[2], raw[3], raw[4], raw[5], raw[6]);
  }

  // Shift into the CST816S layout: reg 0x01 onwards.
  uint8_t b[6];
  for (int i = 0; i < 6; i++) b[i] = raw[i + 1];

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
  if (st == T_FAILED) nFail++; else if (st == T_UP) nUp++; else nDown++;

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
