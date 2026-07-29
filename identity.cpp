// identity.cpp - see identity.h.

#include "identity.h"

#include <Preferences.h>
#include <mbedtls/base64.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/sha256.h>

#include "config.h"

char badgeCode[BADGE_CODE_LEN + 1] = "";
char badgeId[BADGE_ID_LEN + 1] = "";
bool identityReady = false;

static uint8_t badgePub[BADGE_PUB_LEN];
static uint8_t badgePriv[BADGE_PRIV_LEN];

// Crockford base32: no I, L, O or U, so a code read aloud or squinted at off a
// 240px panel cannot be transcribed into a different valid code. That property
// matters more here than the extra four symbols would.
static const char CODE_ALPHABET[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

// esp_random() is the hardware RNG and is only guaranteed non-deterministic
// once an RF clock is running; it is seeded well before setup() on this core.
// Everything generated here is regenerable, so a weak first byte is not fatal.
static void makeCode(char *out) {
  for (int i = 0; i < BADGE_CODE_LEN; i++) {
    out[i] = CODE_ALPHABET[esp_random() % (sizeof(CODE_ALPHABET) - 1)];
  }
  out[BADGE_CODE_LEN] = '\0';
}

// The badge id is a fingerprint of the public key, not a random number. That
// means it cannot be chosen or spoofed independently of the key: if two badges
// ever showed the same id, they would have to share a keypair.
static void deriveId() {
  uint8_t digest[32];
  mbedtls_sha256(badgePub, BADGE_PUB_LEN, digest, 0);
  static const char hex[] = "0123456789abcdef";
  for (int i = 0; i < BADGE_ID_LEN / 2; i++) {
    badgeId[i * 2] = hex[digest[i] >> 4];
    badgeId[i * 2 + 1] = hex[digest[i] & 0x0F];
  }
  badgeId[BADGE_ID_LEN] = '\0';
}

// Generates a P-256 keypair. Returns false on any mbedtls failure, in which
// case the badge still boots - it just has no cryptographic identity and the
// MQTT screen says so, rather than silently pretending to be secure.
static bool generateKeypair() {
  mbedtls_ecdsa_context ecdsa;
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context drbg;
  bool ok = false;

  mbedtls_ecdsa_init(&ecdsa);
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&drbg);

  const char *pers = "dc34-badge-identity";
  if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                            (const uint8_t *)pers, strlen(pers)) == 0 &&
      mbedtls_ecdsa_genkey(&ecdsa, MBEDTLS_ECP_DP_SECP256R1,
                           mbedtls_ctr_drbg_random, &drbg) == 0) {
    size_t olen = 0;
    if (mbedtls_ecp_point_write_binary(
            &ecdsa.MBEDTLS_PRIVATE(grp), &ecdsa.MBEDTLS_PRIVATE(Q),
            MBEDTLS_ECP_PF_UNCOMPRESSED, &olen, badgePub,
            sizeof(badgePub)) == 0 &&
        olen == BADGE_PUB_LEN &&
        mbedtls_mpi_write_binary(&ecdsa.MBEDTLS_PRIVATE(d), badgePriv,
                                 sizeof(badgePriv)) == 0) {
      ok = true;
    }
  }

  mbedtls_ctr_drbg_free(&drbg);
  mbedtls_entropy_free(&entropy);
  mbedtls_ecdsa_free(&ecdsa);
  return ok;
}

void identityBegin() {
  if (identityReady) return;

  Preferences p;
  p.begin("identity", false);

  const size_t havePub = p.getBytesLength("pub");
  const size_t havePriv = p.getBytesLength("priv");
  bool loaded = false;

  if (havePub == BADGE_PUB_LEN && havePriv == BADGE_PRIV_LEN) {
    p.getBytes("pub", badgePub, sizeof(badgePub));
    p.getBytes("priv", badgePriv, sizeof(badgePriv));
    loaded = true;
  } else {
    // Keygen is ~200ms on this part and happens exactly once per badge, on
    // the very first boot, so it is not worth deferring off the boot path.
    const uint32_t t0 = millis();
    if (generateKeypair()) {
      p.putBytes("pub", badgePub, sizeof(badgePub));
      p.putBytes("priv", badgePriv, sizeof(badgePriv));
      loaded = true;
      Serial.printf("identity: generated new keypair in %lums\n",
                    (unsigned long)(millis() - t0));
    } else {
      Serial.println("identity: KEYGEN FAILED - badge has no crypto identity");
    }
  }

  // Check what actually landed in the buffer, not getString()'s return value:
  // Preferences reports the length INCLUDING the NUL, so a stored 5-char code
  // comes back as 6. Comparing that against BADGE_CODE_LEN never matched, and
  // the code was silently regenerated on every single boot - which would have
  // invalidated any pairing the moment a badge was power-cycled.
  badgeCode[0] = '\0';
  p.getString("code", badgeCode, sizeof(badgeCode));
  if (strlen(badgeCode) != BADGE_CODE_LEN) {
    makeCode(badgeCode);
    p.putString("code", badgeCode);
    Serial.println("identity: issued a new pairing code");
  }
  p.end();

  if (loaded) {
    deriveId();
    identityReady = true;
  } else {
    strncpy(badgeId, "--------", sizeof(badgeId));
  }
  LOGF("identity: id=%s code=%s\n", badgeId, badgeCode);
}

void identityRegenerateCode() {
  makeCode(badgeCode);
  Preferences p;
  p.begin("identity", false);
  p.putString("code", badgeCode);
  p.end();
  LOGF("identity: new pairing code %s\n", badgeCode);
}

const char *identityPublicKeyB64() {
  static char b64[128];
  size_t olen = 0;
  if (mbedtls_base64_encode((unsigned char *)b64, sizeof(b64), &olen, badgePub,
                            sizeof(badgePub)) != 0) {
    return "";
  }
  b64[olen < sizeof(b64) ? olen : sizeof(b64) - 1] = '\0';
  return b64;
}
