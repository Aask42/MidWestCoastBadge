// ota.h - over-the-air firmware update, triggered over MQTT.
//
// Flow:
//   1. Something publishes {"ota":"http://host/badge.bin"} to the badge's cmd
//      topic, optionally with "md5":"<hex>".
//   2. The badge acknowledges on its own topic, then downloads and writes the
//      image into the INACTIVE app slot while still running from the active one.
//   3. On success it marks the new slot bootable and reboots into it.
//
// Deliberately blocking. An OTA takes a few seconds, and interleaving it with
// rendering and touch would mean a half-written image if the badge idled into
// a mode mid-download. The screen shows a progress bar so it does not look
// like a crash.
//
// Safety properties worth keeping if this is ever rewritten:
//   - the running image is never overwritten, so a failed download changes
//     nothing and the badge simply carries on
//   - the image is size-checked against the free slot BEFORE any write
//   - an optional MD5 is verified by the Update library before the boot flag
//     is moved, so a truncated or corrupted download cannot be booted into
//   - failures are published, because a badge that silently refuses to update
//     is worse than one that says why

#pragma once

#include <Arduino.h>

// Downloads and installs. Returns only on FAILURE - success reboots.
// `md5hex` may be null to skip verification.
void otaRun(const char *url, const char *md5hex);

// Last failure reason, for the MQTT status document. "" if none.
const char *otaLastError();
