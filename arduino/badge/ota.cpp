// ota.cpp - see ota.h.

#include "ota.h"

#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClient.h>

#include "config.h"
#include "display.h"
#include "store.h"

static char lastError[64] = "";

const char *otaLastError() { return lastError; }

static void fail(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(lastError, sizeof(lastError), fmt, ap);
  va_end(ap);
  LOGF("ota: FAILED - %s\n", lastError);
}

// Full-screen progress. Drawn straight to the panel rather than through the
// strip compositor: the compositor re-runs every draw call four times per
// frame, which is wasted work when the badge is otherwise busy downloading.
static void drawProgress(int pct, const char *note) {
  static int lastPct = -1;
  if (pct == lastPct) return;
  lastPct = pct;

  const int bw = SCREEN_W - 48, bh = 18;
  const int bx = 24, by = SCREEN_H / 2 - bh / 2;

  if (pct <= 0) {
    panel->fillScreen(C_BG);
    printCentered(panel, "UPDATING", 0, by - 46, 2, C_ACCENT);
    printCentered(panel, "do not remove power", 0, by - 22, 1, C_WARN);
  }
  panel->drawRect(bx, by, bw, bh, C_DIM);
  if (pct > 0) {
    panel->fillRect(bx + 2, by + 2, (bw - 4) * pct / 100, bh - 4, C_ACCENT);
  }

  char txt[48];
  snprintf(txt, sizeof(txt), "%d%%  %s", pct, note ? note : "");
  panel->fillRect(0, by + bh + 10, SCREEN_W, 12, C_BG);
  printCentered(panel, txt, 0, by + bh + 10, 1, C_DIM);
}

void otaRun(const char *url, const char *md5hex) {
  lastError[0] = '\0';

  if (WiFi.status() != WL_CONNECTED) {
    fail("no wifi");
    return;
  }
  if (!url || !url[0]) {
    fail("no url");
    return;
  }

  LOGF("ota: fetching %s\n", url);
  drawProgress(0, "connecting");

  WiFiClient client;
  HTTPClient http;
  http.setTimeout(15000);
  // Firmware hosts commonly 302 to a CDN; without this the badge would treat
  // the redirect body as an image and fail the magic-byte check.
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, url)) {
    fail("bad url");
    return;
  }

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    fail("http %d", code);
    http.end();
    return;
  }

  const int len = http.getSize();
  if (len <= 0) {
    // Update.begin() needs a known length to size-check against the slot, and
    // a chunked response does not give one.
    fail("no content-length");
    http.end();
    return;
  }

  LOGF("ota: %d bytes, free slot %u\n", len,
       (unsigned)ESP.getFreeSketchSpace());

  // Checked before a single byte is written. The running image is in the other
  // slot and is never touched, so bailing here costs nothing.
  if (!Update.begin((size_t)len, U_FLASH)) {
    fail("too big: %u free", (unsigned)ESP.getFreeSketchSpace());
    http.end();
    return;
  }
  if (md5hex && md5hex[0]) {
    Update.setMD5(md5hex);
    LOGF("ota: expecting md5 %s\n", md5hex);
  }

  Update.onProgress([](size_t done, size_t total) {
    drawProgress(total ? (int)(done * 100 / total) : 0, "writing");
  });

  const size_t written = Update.writeStream(*http.getStreamPtr());
  http.end();

  if (written != (size_t)len) {
    fail("short write %u/%u", (unsigned)written, (unsigned)len);
    Update.abort();
    return;
  }

  // end(true) verifies the MD5 if one was set and only then flips otadata to
  // the new slot. A corrupt download stops here, still running the old image.
  if (!Update.end(true)) {
    fail("verify failed (%u)", (unsigned)Update.getError());
    return;
  }

  LOGF("ota: installed, rebooting into new slot\n");
  drawProgress(100, "rebooting");
  panel->fillScreen(C_BG);
  printCentered(panel, "UPDATED", 0, SCREEN_H / 2 - 20, 2, C_OK);
  printCentered(panel, "rebooting...", 0, SCREEN_H / 2 + 8, 1, C_DIM);
  Serial.flush();
  delay(1200);
  ESP.restart();
}
