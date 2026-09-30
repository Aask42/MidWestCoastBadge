// ota.h - over-the-air firmware update, triggered over MQTT.
//
// The main application stores the signed command's URL and MD5 in NVS, then
// boots the immutable factory recovery application. Recovery owns networking,
// download, verification, and writing the one large main slot.

#pragma once

#include <Arduino.h>

// Downloads and installs. Returns only on FAILURE - success reboots.
// `md5hex` may be null to skip verification.
void otaRun(const char *url, const char *md5hex);

// Confirms a newly installed main image once all normal subsystems initialize.
void otaConfirmBoot();

// Last failure reason, for the MQTT status document. "" if none.
const char *otaLastError();
