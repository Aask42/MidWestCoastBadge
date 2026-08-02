// authcmd.h - signature verification for MQTT commands.
//
// Badges trust exactly one public key, baked in at build time by
// tools/make_operator_key.py. Anything arriving on a cmd topic must carry a
// valid signature from the matching private key or it is dropped unread.
//
// This matters most for OTA: without it, anyone who can publish to the broker
// can install arbitrary firmware on every badge. Renaming a badge is a prank;
// flashing it is not.
//
// Wire format, deliberately not JSON-in-JSON - nesting would mean un-escaping
// a string inside a string, which is exactly where parser bugs live:
//
//     v1\n<seq>\n<base64 DER signature>\n<command json>
//
// The signature covers "v1\n<seq>\n<command json>" - everything but the
// signature line. `seq` is monotonic and persisted, so a captured message
// cannot be replayed later.

#pragma once

#include <Arduino.h>

void authBegin();

// Verifies `payload` and, on success, points `body`/`bodyLen` at the command
// JSON inside it. Returns false and logs a reason otherwise.
bool authVerify(const char *payload, size_t len, const char **body,
                size_t *bodyLen);

// Fingerprint of the trusted key, for the status doc and the serial banner.
const char *authFingerprint();
