// ota.cpp - see ota.h.

#include "ota.h"

#include <Preferences.h>
#include <esp_ota_ops.h>

#include "config.h"
#include "display.h"

static char lastError[64] = "";

const char *otaLastError() { return lastError; }

static void fail(const char *message) {
  snprintf(lastError, sizeof(lastError), "%s", message);
  LOGF("ota: FAILED - %s\n", lastError);
}

void otaConfirmBoot() {
  Preferences request;
  request.begin("recovery", false);
  if (request.getString("phase", "") == "booting") {
    const esp_err_t result = esp_ota_mark_app_valid_cancel_rollback();
    if (result == ESP_OK) {
      request.clear();
      LOGF("ota: new main firmware confirmed\n");
    } else {
      LOGF("ota: could not confirm main firmware (%d)\n", (int)result);
    }
  }
  request.end();
}

void otaRun(const char *url, const char *md5hex) {
  lastError[0] = '\0';
  if (!url || !url[0]) {
    fail("no url");
    return;
  }
  if (strncmp(url, "http://", 7) != 0) {
    fail("recovery requires http");
    return;
  }
  if (strlen(url) >= 160) {
    fail("url too long");
    return;
  }
  if (md5hex && md5hex[0] && strlen(md5hex) != 32) {
    fail("bad md5");
    return;
  }

  const esp_partition_t *recovery = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
  if (!recovery) {
    fail("recovery missing");
    return;
  }

  Preferences request;
  if (!request.begin("recovery", false)) {
    fail("cannot save request");
    return;
  }
  request.clear();
  const char *md5 = md5hex ? md5hex : "";
  const bool saved = request.putString("url", url) == strlen(url) &&
                     request.putString("md5", md5) == strlen(md5) &&
                     request.putString("phase", "download") == 8;
  request.end();
  if (!saved) {
    fail("cannot save request");
    return;
  }

  if (esp_ota_set_boot_partition(recovery) != ESP_OK) {
    fail("cannot boot recovery");
    return;
  }

  LOGF("ota: request saved, rebooting into recovery\n");
  panel->fillScreen(C_BG);
  printCentered(panel, "UPDATE READY", 0, SCREEN_H / 2 - 24, 2, C_OK);
  printCentered(panel, "starting recovery...", 0, SCREEN_H / 2 + 8, 1,
                C_DIM);
  Serial.flush();
  delay(750);
  ESP.restart();
}
