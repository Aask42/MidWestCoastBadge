// mqtt.cpp - see mqtt.h.

#include "mqtt.h"

#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

#include "config.h"
#include "display.h"
#include "identity.h"
#include "menus.h"
#include "net.h"
#include "authcmd.h"
#include "ota.h"
#include "power.h"
#include "modes.h"
#include "store.h"
#include "ui.h"

// Two transports, one client. Which one is handed to PubSubClient is decided
// by the port at connect time.
static WiFiClient plainClient;
static WiFiClientSecure tlsClient;
static PubSubClient mqtt;

static bool usingTls = false;
static uint32_t lastAttempt = 0;
static uint8_t failures = 0;
static char lastError[32] = "";

// Backoff. A broker that is down should not be hammered, and a wrong password
// should not spin the radio flat - but a transient drop should recover fast.
static const uint32_t RETRY_MIN_MS = 3000;
static const uint32_t RETRY_MAX_MS = 60000;

static uint32_t retryDelay() {
  uint32_t d = RETRY_MIN_MS << (failures > 4 ? 4 : failures);
  return d > RETRY_MAX_MS ? RETRY_MAX_MS : d;
}

// Topic roots. iotTopic is the user-facing "topic" field and acts as a
// namespace, so two groups on one broker do not collide.
static void topicFor(char *out, size_t n, const char *leaf) {
  snprintf(out, n, "%s/badge/%s/%s", iotTopic[0] ? iotTopic : "dc34", badgeId,
           leaf);
}

const char *mqttStateText() {
  if (mqtt.connected()) return "connected";
  if (lastError[0]) return lastError;
  if (!wifiIsConnected()) return "no wifi";
  if (iotBroker[0] == '\0') return "no broker set";
  return "connecting";
}

bool mqttIsConnected() { return mqtt.connected(); }

// PubSubClient's numeric states are not useful in a UI, so they are mapped to
// something a person can act on. -2 vs -4 is the difference between "wrong
// address" and "broker is there but slow", which matters when debugging.
static const char *errText(int rc) {
  switch (rc) {
    case -4: return "timeout";
    case -3: return "conn lost";
    case -2: return "connect failed";
    case -1: return "disconnected";
    case 1: return "bad protocol";
    case 2: return "id rejected";
    case 3: return "broker down";
    case 4: return "bad user/pw";
    case 5: return "not authorised";
    default: return "error";
  }
}

void mqttPublishState() {
  if (!mqtt.connected()) return;

  char topic[96];
  topicFor(topic, sizeof(topic), "state");

  // Deliberately not including the pairing secret. It is shown on the badge's
  // own screen precisely so it never has to travel over the wire.
  char payload[256];
  snprintf(payload, sizeof(payload),
           "{\"id\":\"%s\",\"name\":\"%s\",\"show\":\"%s\",\"v\":\"%s\"}",
           badgeId, nametagName, modeItems[activeMode()], BADGE_VERSION);

  // Retained: a client that connects later still learns this badge exists
  // without having to wait for the next change.
  mqtt.publish(topic, payload, true);
  LOGF("mqtt: published state to %s\n", topic);
}

// Commands in. Plaintext JSON for now - the sealed envelope from PAIRING.md
// replaces this, at which point unauthenticated payloads get dropped here.
static void onMessage(char *topic, uint8_t *payload, unsigned int len) {
  // Verify BEFORE parsing anything. An unsigned command is not a command, and
  // the parser below should never see attacker-controlled bytes that have not
  // been authenticated first.
  const char *body = nullptr;
  size_t bodyLen = 0;
  if (!authVerify((const char *)payload, len, &body, &bodyLen)) {
    LOGF("mqtt: DROPPED unsigned/invalid command on %s\n", topic);
    return;
  }

  char buf[256];
  const size_t n = bodyLen < sizeof(buf) - 1 ? bodyLen : sizeof(buf) - 1;
  memcpy(buf, body, n);
  buf[n] = '\0';
  LOGF("mqtt: rx %s -> %s (signed ok)\n", topic, buf);

  // banner: {"msg":"TEXT","secs":30} - the one command that is useful to send
  // to every badge at once.
  const char *bp = strstr(buf, "\"msg\"");
  if (bp) {
    const char *q = strchr(bp + 5, '"');
    if (q) {
      const char *e = strchr(++q, '"');
      if (e) {
        char text[96];
        size_t tl = (size_t)(e - q);
        if (tl >= sizeof(text)) tl = sizeof(text) - 1;
        memcpy(text, q, tl);
        text[tl] = '\0';
        uint32_t secs = 20;
        const char *sp2 = strstr(buf, "\"secs\"");
        if (sp2) {
          const char *c = strchr(sp2 + 6, ':');
          if (c) secs = (uint32_t)atoi(c + 1);
        }
        if (secs < 1) secs = 1;
        if (secs > 600) secs = 600;
        bannerShow(text, secs);
      }
    }
    return;
  }

  // ota: {"ota":"http://host/badge.bin","md5":"<hex>"}. Handled first and
  // returns regardless, because otaRun() either reboots or leaves an error
  // worth reporting - there is nothing sensible to do after it in either case.
  const char *op = strstr(buf, "\"ota\"");
  if (op) {
    char url[160] = "", md5[40] = "";
    const char *q = strchr(op + 5, '"');
    if (q) {
      const char *e = strchr(++q, '"');
      if (e && (size_t)(e - q) < sizeof(url)) {
        memcpy(url, q, e - q);
        url[e - q] = '\0';
      }
    }
    const char *mp = strstr(buf, "\"md5\"");
    if (mp) {
      const char *m = strchr(mp + 5, '"');
      if (m) {
        const char *e = strchr(++m, '"');
        if (e && (size_t)(e - m) < sizeof(md5)) {
          memcpy(md5, m, e - m);
          md5[e - m] = '\0';
        }
      }
    }

    // Tell the world before starting: the badge is about to stop servicing
    // MQTT for several seconds and then reboot, and a silent gap looks
    // identical to a crash.
    char t2[96];
    topicFor(t2, sizeof(t2), "state");
    char msg[224];
    snprintf(msg, sizeof(msg), "{\"id\":\"%s\",\"ota\":\"starting\",\"url\":\"%s\"}",
             badgeId, url);
    mqtt.publish(t2, msg, true);
    mqtt.loop();

    otaRun(url, md5[0] ? md5 : nullptr);

    // Only reached on failure - success reboots inside otaRun().
    snprintf(msg, sizeof(msg), "{\"id\":\"%s\",\"ota\":\"failed\",\"err\":\"%s\"}",
             badgeId, otaLastError());
    mqtt.publish(t2, msg, true);
    render();
    return;
  }

  // setShow: pick what the badge displays, by index into the SHOW menu.
  const char *sp = strstr(buf, "\"setShow\"");
  if (sp) {
    sp = strchr(sp + 9, ':');
    if (sp) {
      const int v = atoi(sp + 1);
      if (v >= 0 && v < (int)menus[MENU_MODE].count) {
        updateMenuSelection(menus[MENU_MODE], (uint8_t)v);
        saveSettings();
        modeActive = true;
        modesEnter(millis());
        LOGF("mqtt: show set to %s\n", modeItems[activeMode()]);
        mqttPublishState();
        render();
      }
    }
    return;
  }

  // Minimal parse. A real JSON parser is not worth 20KB for two fields; this
  // is replaced wholesale when the envelope format lands.
  const char *p = strstr(buf, "\"setName\"");
  if (!p) return;
  p = strchr(p + 9, ':');
  if (!p) return;
  p = strchr(p, '"');
  if (!p) return;
  const char *end = strchr(++p, '"');
  if (!end) return;

  char name[sizeof(nametagName)];
  size_t len2 = (size_t)(end - p);
  if (len2 >= sizeof(name)) len2 = sizeof(name) - 1;
  memcpy(name, p, len2);
  name[len2] = '\0';
  if (!name[0]) return;

  setField(nametagName, sizeof(nametagName), name);
  saveSettings();
  LOGF("mqtt: name set to '%s'\n", nametagName);
  mqttPublishState();
  render();
}

void mqttBegin() {
  mqtt.setCallback(onMessage);
  // Default is 256 bytes, which the state document plus a long name can
  // outgrow silently - publish() just returns false.
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(30);
}

static void connectOnce() {
  const uint16_t port = (uint16_t)atoi(iotPort);
  usingTls = (port == 8883 || port == 8884);

  if (usingTls) {
    // TODO before this is trusted with anything: pin HiveMQ's CA rather than
    // accepting any certificate. setInsecure() means the connection is
    // encrypted but NOT authenticated - fine on a bench, not fine in public.
    tlsClient.setInsecure();
    mqtt.setClient(tlsClient);
  } else {
    mqtt.setClient(plainClient);
  }
  mqtt.setServer(iotBroker, port);

  char willTopic[96];
  topicFor(willTopic, sizeof(willTopic), "state");

  LOGF("mqtt: connecting to %s:%u over %s as %s\n", iotBroker, (unsigned)port,
       usingTls ? "TLS" : "TCP", iotClientId);

  // The will clears the retained state doc, so a badge that drops off does not
  // leave a stale "I am here" behind it.
  const bool ok =
      iotUser[0]
          ? mqtt.connect(iotClientId, iotUser, iotPass, willTopic, 0, true, "")
          : mqtt.connect(iotClientId, willTopic, 0, true, "");

  if (ok) {
    failures = 0;
    lastError[0] = '\0';
    char cmdTopic[96];
    topicFor(cmdTopic, sizeof(cmdTopic), "cmd");
    mqtt.subscribe(cmdTopic);

    // Broadcast: one signed message reaches every badge at once. Same
    // verification path, so it is no less authenticated than a direct command.
    char allTopic[96];
    snprintf(allTopic, sizeof(allTopic), "%s/all/cmd",
             iotTopic[0] ? iotTopic : "dc34");
    mqtt.subscribe(allTopic);
    LOGF("mqtt: connected, subscribed %s and %s\n", cmdTopic, allTopic);
    mqttPublishState();
  } else {
    const int rc = mqtt.state();
    snprintf(lastError, sizeof(lastError), "%s", errText(rc));
    if (failures < 255) failures++;
    LOGF("mqtt: connect failed rc=%d (%s), retry in %lums\n", rc, lastError,
         (unsigned long)retryDelay());
  }
}

// Republish whenever what we last told the broker stops being true. Doing it
// here rather than at each call site means every path that can change the
// badge - a menu tap, the serial console, an MQTT command, a factory reset -
// is covered without anyone having to remember to publish. The retained doc
// otherwise goes stale the moment someone uses the badge's own screen.
static void publishIfChanged() {
  static char lastName[sizeof(nametagName)] = "";
  static uint8_t lastShow = 0xFF;
  if (!mqtt.connected()) return;
  if (strcmp(lastName, nametagName) == 0 && lastShow == activeMode()) return;
  setField(lastName, sizeof(lastName), nametagName);
  lastShow = activeMode();
  mqttPublishState();
}

// Runtime telemetry. NOT retained: these are samples over time, and a retained
// sample would sit on the broker as a stale reading forever. The last one
// received before a badge goes quiet is how long that pack lasted.
static uint32_t lastTelemetry = 0;

static void publishTelemetry() {
  if (!mqtt.connected()) return;
  char topic[96];
  topicFor(topic, sizeof(topic), "telemetry");

  char payload[224];
  const int pct = batteryPercent();
  snprintf(payload, sizeof(payload),
           "{\"id\":\"%s\",\"up\":%lu,\"mv\":%u,\"pct\":%d,"
           "\"rssi\":%d,\"heap\":%lu,\"show\":\"%s\",\"bright\":%u,"
           "\"clock\":\"%s\",\"shows\":%u,\"v\":\"%s\"}",
           badgeId, (unsigned long)uptimeSeconds(),
           (unsigned)batteryMillivolts(), pct, (int)WiFi.RSSI(),
           (unsigned long)ESP.getFreeHeap(), modeItems[activeMode()],
           (unsigned)BRIGHT_PCT[brightness],
           timeIsSynced() ? "ntp" : "uptime", (unsigned)SHOW_COUNT,
           BADGE_VERSION);
  mqtt.publish(topic, payload, false);
}

void mqttTick(uint32_t now) {
  const bool wasOnline = iotOnline;

  if (!wifiIsConnected() || iotBroker[0] == '\0') {
    if (mqtt.connected()) mqtt.disconnect();
    iotOnline = false;
  } else if (mqtt.connected()) {
    mqtt.loop();
    publishIfChanged();
    if (now - lastTelemetry >= TELEMETRY_MS) {
      lastTelemetry = now;
      publishTelemetry();
    }
    iotOnline = true;
  } else {
    iotOnline = false;
    if (now - lastAttempt >= retryDelay()) {
      lastAttempt = now;
      connectOnce();
    }
  }

  if (iotOnline != wasOnline) {
    refreshIotLabels();
    if (current == MENU_IOT) render();
  }
}
