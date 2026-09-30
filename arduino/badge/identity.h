// identity.h - who this badge is, cryptographically.
//
// Two separate things live here and they must not be confused:
//
//   badgePub / badgePriv  A P-256 keypair generated once on first boot and
//                         kept in NVS for the life of the badge. The public
//                         half is the badge's durable identity - it is what
//                         lets a message be attributed to THIS badge and no
//                         other. The private half never leaves the device.
//
//   badgeCode             A five-character secret shown on the MQTT screen.
//                         This is NOT a cryptographic key. It is a pairing
//                         token: proof that whoever is typing it physically
//                         held the badge. Five characters of Crockford base32
//                         is ~25 bits, which is fine for a one-shot pairing
//                         that is rate-limited and expires, and nowhere near
//                         enough to protect anything long-lived. See
//                         docs/PAIRING.md for the threat model and why the
//                         claim flow is built the way it is.
//
// The pairing code authenticates the FIRST contact; the keypair authenticates
// everything afterwards.

#pragma once

#include <Arduino.h>

#define BADGE_CODE_LEN 5
#define BADGE_ID_LEN 8   // hex chars of the public key fingerprint
#define BADGE_PUB_LEN 65 // uncompressed P-256 point: 0x04 || X || Y
#define BADGE_PRIV_LEN 32

extern char badgeCode[BADGE_CODE_LEN + 1];
extern char badgeId[BADGE_ID_LEN + 1];
extern bool identityReady;  // false if keygen failed; badge still runs

// Loads the keypair and pairing code from NVS, generating either if absent.
// Safe to call once from setup(); does nothing on later calls.
void identityBegin();

// Issues a fresh pairing code. Invalidates any pairing in flight, which is the
// point - this is how a user revokes a code they read out loud by mistake.
void identityRegenerateCode();

// Base64 of the public key, for display or for handing to the web client.
// Returns a pointer to a static buffer, overwritten on each call.
const char *identityPublicKeyB64();
