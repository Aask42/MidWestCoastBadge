// net.h - WiFi association.
//
// Everything here is asynchronous. WiFi.begin() returns immediately and status
// is polled, so a wrong passphrase costs a status label rather than freezing
// the UI for the length of a join timeout.
//
// The MQTT client is not implemented yet - see docs/PAIRING.md for the design
// it has to satisfy. iotOnline in store.h stays false until it lands.

#pragma once

#include <Arduino.h>

void netBegin();          // auto-joins if an SSID is already stored
void wifiConnect();
void wifiDisconnect();
const char *wifiStateText();
bool wifiIsConnected();
const char *wifiIpText();

// Asynchronous scan used by the WiFi scanner display. Results are copied out
// of the driver so rendering and MQTT publication never hold its scan buffer.
bool wifiScanStart();
bool wifiScanTick();
void wifiScanModeEnter();
void wifiScanModeExit();
bool wifiScanRunning();
uint8_t wifiScanCount();
const char *wifiScanSsid(uint8_t index);
int8_t wifiScanRssi(uint8_t index);
bool wifiScanSecure(uint8_t index);
uint8_t wifiScanChannel(uint8_t index);
const char *wifiScanBssid(uint8_t index);
const char *wifiScanAuth(uint8_t index);
uint16_t wifiScanSessionCount();
uint32_t wifiScanGeneration();

// Called from loop(). Returns true if the status changed, so the caller can
// decide whether a redraw is warranted.
bool netTick(uint32_t now);
