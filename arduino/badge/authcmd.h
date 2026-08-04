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

// Owner commands use the code printed on the badge rather than the fleet
// operator key. They are accepted only on the per-badge /owner topic and are
// restricted by mqtt.cpp to name, show, and popup operations.
//
//     o1\n<seq>\n<base64 HMAC-SHA256>\n<command json>
//
// The HMAC covers "o1\n<seq>\n<command json>". Its key is derived once from
// badgeCode with PBKDF2-HMAC-SHA256(100000), salted by badgeId.
bool authVerifyOwner(const char *payload, size_t len, const char **body,
                     size_t *bodyLen);

// Re-derives the cached owner key and clears its replay counter after the code
// is rotated from the badge screen.
void authOwnerCodeChanged();

// Fingerprint of the trusted key, for the status doc and the serial banner.
const char *authFingerprint();
