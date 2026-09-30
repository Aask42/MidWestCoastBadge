#include "ble.h"

#include <NimBLEDevice.h>

#include "identity.h"

namespace {

constexpr uint8_t BADGE_FRAME[] = {'D', 'C', '3', '4', 1};
constexpr uint8_t PEER_MAX = 32;
constexpr uint32_t PEER_NEARBY_MS = 30000;
constexpr uint32_t SCAN_RESTART_MS = 30000;

struct Peer {
  uint32_t id;
  uint32_t seenAt;
  int8_t rssi;
};

Peer peers[PEER_MAX] = {};
uint8_t peerCount = 0;
uint16_t sessionCount = 0;
portMUX_TYPE peerMux = portMUX_INITIALIZER_UNLOCKED;
NimBLEScan *scanner = nullptr;

uint32_t parseBadgeId(const char *text) {
  uint32_t value = 0;
  for (uint8_t i = 0; i < BADGE_ID_LEN; i++) {
    const char c = text[i];
    value <<= 4;
    value |= c >= '0' && c <= '9' ? c - '0'
             : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                    : c - 'A' + 10;
  }
  return value;
}

void recordPeer(uint32_t id, int8_t rssi) {
  if (id == parseBadgeId(badgeId)) return;
  const uint32_t now = millis();

  portENTER_CRITICAL(&peerMux);
  for (uint8_t i = 0; i < peerCount; i++) {
    if (peers[i].id == id) {
      peers[i].seenAt = now;
      peers[i].rssi = rssi;
      portEXIT_CRITICAL(&peerMux);
      return;
    }
  }

  uint8_t slot = peerCount;
  if (peerCount < PEER_MAX) {
    peerCount++;
    sessionCount++;
  } else {
    slot = 0;
    for (uint8_t i = 1; i < PEER_MAX; i++) {
      if ((int32_t)(peers[i].seenAt - peers[slot].seenAt) < 0) slot = i;
    }
  }
  peers[slot] = {id, now, rssi};
  portEXIT_CRITICAL(&peerMux);
}

class BadgeScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice *device) override {
    if (!device->haveManufacturerData()) return;
    const std::string data = device->getManufacturerData();
    if (data.size() != sizeof(BADGE_FRAME) + sizeof(uint32_t) ||
        memcmp(data.data(), BADGE_FRAME, sizeof(BADGE_FRAME)) != 0) {
      return;
    }

    const uint8_t *raw = reinterpret_cast<const uint8_t *>(data.data()) +
                         sizeof(BADGE_FRAME);
    const uint32_t id = (uint32_t)raw[0] << 24 | (uint32_t)raw[1] << 16 |
                        (uint32_t)raw[2] << 8 | raw[3];
    recordPeer(id, device->getRSSI());
  }

  void onScanEnd(const NimBLEScanResults &, int) override {
    if (scanner) scanner->start(SCAN_RESTART_MS, false, true);
  }
} scanCallbacks;

}  // namespace

void bleBegin() {
  if (!identityReady) return;

  NimBLEDevice::init("");

  const uint32_t id = parseBadgeId(badgeId);
  uint8_t payload[sizeof(BADGE_FRAME) + sizeof(id)];
  memcpy(payload, BADGE_FRAME, sizeof(BADGE_FRAME));
  payload[5] = id >> 24;
  payload[6] = id >> 16;
  payload[7] = id >> 8;
  payload[8] = id;

  NimBLEAdvertisementData advertisement;
  advertisement.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  advertisement.setManufacturerData(payload, sizeof(payload));
  NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
  advertising->setConnectableMode(BLE_GAP_CONN_MODE_NON);
  advertising->enableScanResponse(false);
  advertising->setAdvertisingInterval(1600);  // 1 second, in 0.625 ms units.
  advertising->setAdvertisementData(advertisement);
  advertising->start();

  scanner = NimBLEDevice::getScan();
  scanner->setScanCallbacks(&scanCallbacks, true);
  scanner->setActiveScan(false);
  scanner->setMaxResults(0);
  scanner->setInterval(1600);
  scanner->setWindow(100);
  scanner->start(SCAN_RESTART_MS, false, true);
}

void bleTick(uint32_t) {}

uint8_t bleNearbyCount() {
  const uint32_t now = millis();
  uint8_t count = 0;
  portENTER_CRITICAL(&peerMux);
  for (uint8_t i = 0; i < peerCount; i++) {
    if (now - peers[i].seenAt <= PEER_NEARBY_MS) count++;
  }
  portEXIT_CRITICAL(&peerMux);
  return count;
}

uint16_t bleSessionCount() {
  portENTER_CRITICAL(&peerMux);
  const uint16_t count = sessionCount;
  portEXIT_CRITICAL(&peerMux);
  return count;
}

uint8_t blePeerCount() { return peerCount; }

const char *blePeerId(uint8_t index) {
  static char text[BADGE_ID_LEN + 1];
  portENTER_CRITICAL(&peerMux);
  const uint32_t id = index < peerCount ? peers[index].id : 0;
  portEXIT_CRITICAL(&peerMux);
  snprintf(text, sizeof(text), "%08lX", (unsigned long)id);
  return text;
}

int8_t blePeerRssi(uint8_t index) {
  portENTER_CRITICAL(&peerMux);
  const int8_t rssi = index < peerCount ? peers[index].rssi : -127;
  portEXIT_CRITICAL(&peerMux);
  return rssi;
}

uint32_t blePeerAgeMs(uint8_t index) {
  portENTER_CRITICAL(&peerMux);
  const uint32_t seenAt = index < peerCount ? peers[index].seenAt : 0;
  portEXIT_CRITICAL(&peerMux);
  return millis() - seenAt;
}