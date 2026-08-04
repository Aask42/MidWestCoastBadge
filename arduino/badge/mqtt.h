// mqtt.h - MQTT client.
//
// Transport is chosen by port, not by a separate toggle:
//   1883  plain TCP      - a local mosquitto on the bench
//   8883  TLS            - HiveMQ Cloud and anything else public-facing
//
// That means the same firmware moves from the bench broker to HiveMQ by editing
// two fields on the MQTT screen, with no reflash.
//
// Everything is non-blocking. A broker that is down, wrong, or refusing auth
// costs a status label and a retry timer - never a frozen UI.
//
// Topics (see ../../PAIRING.md for where this is going):
//   <root>/badge/<id>/state   retained; who this badge is and what it shows
//   <root>/badge/<id>/cmd     subscribed; commands in
//
// The payloads are plaintext JSON for now. The sealed-envelope scheme in
// PAIRING.md replaces that, and is why `secret` is already on the MQTT screen.

#pragma once

#include <Arduino.h>

void mqttBegin();
void mqttTick(uint32_t now);

// True once connected and subscribed. Mirrors into iotOnline for the UI.
bool mqttIsConnected();
bool mqttIsConnecting();

// Human-readable connection state for the MQTT menu's status row.
const char *mqttStateText();

// Publishes the retained state document. Safe to call when disconnected.
void mqttPublishState();

// Publishes the most recent WiFi scan once per generation when sharing is
// enabled. Private mode clears any retained scan on the next connection.
void mqttPublishWifiScan();
