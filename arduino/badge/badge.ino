// badge.ino - DEF CON 34 badge firmware: entry point and wiring only.
//
// production flash: python3 tools/mass_flash.py --once
// main-only compile: arduino-cli compile
//   --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc,FlashSize=4M
//   --build-property upload.maximum_size=2162688 arduino/badge
//
// See README.md for the build flags, flash layout, gesture map and module
// tour, and docs/PAIRING.md for the badge-identity and web-client design.
//
// Everything real lives in the modules; this file exists to bring them up in
// the right order and to pump them. If logic accumulates here, it belongs in
// one of the modules instead.
//
//   config.h    pins, geometry, palette, tuning constants. No code.
//   types.h     shared enums and the Menu struct.
//   display     panel, strip compositor, drawing primitives, transitions.
//   input       CST816S touch and gesture recognition.
//   store       everything persisted to NVS, and the two write policies.
//   identity    the badge keypair and the pairing secret.
//   net         WiFi association, asynchronous throughout.
//   mqtt        MQTT client. Plain TCP on 1883, TLS on 8883 (HiveMQ).
//   power       battery sense and runtime telemetry.
//   ota         MQTT-triggered firmware update into the spare app slot.
//   authcmd     signature check on every inbound MQTT command.
//   keyboard    the on-screen keyboard, shared by every text field.
//   menus       the menu model, its rendering, and the nav stack.
//   modes       the full-screen idle displays.
//   ui          home screen, screen state, and what a gesture MEANS.
//               Tap on home runs the selected mode; tap in a mode returns.
//   console     serial provisioning, so a new badge does not have to be set
//               up by typing a passphrase on a 240px keyboard.

#include "authcmd.h"
#include "ble.h"
#include "config.h"
#include "console.h"
#include "display.h"
#include "identity.h"
#include "input.h"
#include "keyboard.h"
#include "menus.h"
#include "modes.h"
#include "mqtt.h"
#include "net.h"
#include "ota.h"
#include "power.h"
#include "store.h"
#include "types.h"
#include "ui.h"

void setup() {
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
  // Critical: with USB CDC, once the TX buffer fills and no host is reading,
  // every write blocks on this timeout. That is what makes the UI start snappy
  // and then progressively lag as log lines accumulate. 0 = never block, drop
  // output instead. Only HWCDC has this knob, hence the guard.
  Serial.setTxTimeoutMs(0);
#endif
  delay(200);
  Serial.println("badge booting v" BADGE_VERSION);

  displayBegin();
  touchBegin();

  // Identity before settings: the MQTT menu labels render the pairing code,
  // and loadSettings() builds those labels.
  imagesBegin();  // before loadSettings: the SHOW menu counts them
  identityBegin();
  authBegin();  // owner key derivation depends on identity and runs async
  loadSettings();
  displayApplyBrightness();  // only meaningful once the stored level is known

  bleBegin();
  netBegin();
  mqttBegin();
  powerBegin();

  lastActivity = millis();
  render();
  otaConfirmBoot();
}

void loop() {
  // A gesture caught mid-redraw takes priority; otherwise poll fresh.
  Gesture g = pendingGesture;
  if (g != G_NONE) {
    pendingGesture = G_NONE;
  } else {
    g = pollGesture();
  }
  if (g != G_NONE) {
    const uint32_t t0 = millis();
    handleGesture(g);
    LOGF("gesture %d -> screen %d, %lums\n", (int)g, current,
         (unsigned long)(millis() - t0));
  }

  const uint32_t now = millis();
  uiTick(now);
  touchDiag(now);
  storeTick(now);
  powerTick(now);
  bleTick(now);
  consoleTick();
  mqttTick(now);

  // A WiFi state change only matters on screen if SYSTEM or MQTT is showing;
  // both render its status.
  if (netTick(now)) {
    refreshIotLabels();
    if (current == MENU_SYSTEM || current == MENU_IOT) render();
  }

  delay(2);
}
