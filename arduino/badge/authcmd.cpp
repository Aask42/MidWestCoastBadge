// authcmd.cpp - see authcmd.h.

#include "authcmd.h"

#include <Preferences.h>
#include <mbedtls/base64.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>
#include <mbedtls/sha256.h>

#include "config.h"
#include "identity.h"
#include "operator_pub.h"

// Highest sequence number accepted so far. Persisted, because a badge that
// forgot it across a reboot would accept a replay of anything captured before
// that reboot.
static uint32_t lastSeq = 0;
static uint32_t lastOwnerSeq = 0;
static uint8_t ownerKey[32];
static volatile bool ownerKeyReady = false;
static volatile bool ownerDeriveRequested = false;
static volatile bool ownerDeriveInProgress = false;
static portMUX_TYPE ownerDeriveMux = portMUX_INITIALIZER_UNLOCKED;

static bool constantTimeEqual(const uint8_t *a, const uint8_t *b, size_t len) {
  uint8_t difference = 0;
  for (size_t i = 0; i < len; i++) difference |= a[i] ^ b[i];
  return difference == 0;
}

static void ownerDeriveWorker(void *parameter) {
  (void)parameter;

  while (true) {
    portENTER_CRITICAL(&ownerDeriveMux);
    if (!ownerDeriveRequested) {
      ownerDeriveInProgress = false;
      portEXIT_CRITICAL(&ownerDeriveMux);
      break;
    }
    ownerDeriveRequested = false;
    portEXIT_CRITICAL(&ownerDeriveMux);

    char code[BADGE_CODE_LEN + 1];
    char id[BADGE_ID_LEN + 1];
    snprintf(code, sizeof(code), "%s", badgeCode);
    snprintf(id, sizeof(id), "%s", badgeId);

    uint8_t derived[sizeof(ownerKey)];
    char salt[48];
    snprintf(salt, sizeof(salt), "dc34-owner-v1:%s", id);
    const uint32_t t0 = millis();
    const int rc = mbedtls_pkcs5_pbkdf2_hmac_ext(
        MBEDTLS_MD_SHA256, (const uint8_t *)code, strlen(code),
        (const uint8_t *)salt, strlen(salt), 100000, sizeof(derived), derived);
    if (rc == 0 && strcmp(code, badgeCode) == 0 && strcmp(id, badgeId) == 0) {
      memcpy(ownerKey, derived, sizeof(ownerKey));
      ownerKeyReady = true;
      LOGF("auth: owner key ready in %lums\n",
           (unsigned long)(millis() - t0));
    } else if (rc != 0) {
      LOGF("auth: owner key derivation failed (%d)\n", rc);
    }
  }

  vTaskDelete(nullptr);
}

static void deriveOwnerKeyAsync() {
  bool startTask = false;
  ownerKeyReady = false;
  portENTER_CRITICAL(&ownerDeriveMux);
  ownerDeriveRequested = true;
  if (!ownerDeriveInProgress) {
    ownerDeriveInProgress = true;
    startTask = true;
  }
  portEXIT_CRITICAL(&ownerDeriveMux);

  if (!startTask) return;
  if (xTaskCreate(ownerDeriveWorker, "owner-kdf", 4096, nullptr, 1, nullptr) !=
      pdPASS) {
    portENTER_CRITICAL(&ownerDeriveMux);
    ownerDeriveInProgress = false;
    portEXIT_CRITICAL(&ownerDeriveMux);
    LOGF("auth: could not create owner key task\n");
  }
}

void authBegin() {
  Preferences p;
  p.begin("auth", true);
  lastSeq = p.getULong("seq", 0);
  lastOwnerSeq = p.getULong("oseq", 0);
  p.end();
    deriveOwnerKeyAsync();
    LOGF("auth: trusting key %s, seq %lu, owner seq %lu\n",
      OPERATOR_FINGERPRINT, (unsigned long)lastSeq,
      (unsigned long)lastOwnerSeq);
}

const char *authFingerprint() { return OPERATOR_FINGERPRINT; }

static void rememberSeq(uint32_t seq) {
  lastSeq = seq;
  Preferences p;
  p.begin("auth", false);
  p.putULong("seq", seq);
  p.end();
}

static void rememberOwnerSeq(uint32_t seq) {
  lastOwnerSeq = seq;
  Preferences p;
  p.begin("auth", false);
  p.putULong("oseq", seq);
  p.end();
}

void authOwnerCodeChanged() {
  lastOwnerSeq = 0;
  Preferences p;
  p.begin("auth", false);
  p.putULong("oseq", 0);
  p.end();
  deriveOwnerKeyAsync();
}

bool authVerifyOwner(const char *payload, size_t len, const char **body,
                     size_t *bodyLen) {
  if (!ownerKeyReady) return false;
  const char *end = payload + len;
  const char *n1 = (const char *)memchr(payload, '\n', len);
  if (!n1 || n1 - payload != 2 || strncmp(payload, "o1", 2) != 0) return false;
  const char *seqStart = n1 + 1;
  const char *n2 = (const char *)memchr(seqStart, '\n', end - seqStart);
  if (!n2) return false;
  const char *macStart = n2 + 1;
  const char *n3 = (const char *)memchr(macStart, '\n', end - macStart);
  if (!n3) return false;
  const char *bodyStart = n3 + 1;
  const size_t bodyLength = (size_t)(end - bodyStart);

  char seqText[16];
  const size_t seqLength = (size_t)(n2 - seqStart);
  if (!seqLength || seqLength >= sizeof(seqText)) return false;
  memcpy(seqText, seqStart, seqLength);
  seqText[seqLength] = '\0';
  char *seqEnd = nullptr;
  const unsigned long parsed = strtoul(seqText, &seqEnd, 10);
  if (!seqEnd || *seqEnd || parsed > UINT32_MAX || parsed <= lastOwnerSeq) {
    LOGF("auth: owner replay/sequence rejected\n");
    return false;
  }
  const uint32_t seq = (uint32_t)parsed;

  uint8_t supplied[32];
  size_t suppliedLength = 0;
  if (mbedtls_base64_decode(supplied, sizeof(supplied), &suppliedLength,
                            (const uint8_t *)macStart,
                            (size_t)(n3 - macStart)) != 0 ||
      suppliedLength != sizeof(supplied)) {
    return false;
  }

  const size_t prefixLength = (size_t)(n2 - payload + 1);
  if (prefixLength + bodyLength > 512) return false;
  uint8_t signedMessage[512];
  memcpy(signedMessage, payload, prefixLength);
  memcpy(signedMessage + prefixLength, bodyStart, bodyLength);

  uint8_t expected[32];
  const mbedtls_md_info_t *sha256 = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!sha256 || mbedtls_md_hmac(sha256, ownerKey, sizeof(ownerKey),
                                 signedMessage, prefixLength + bodyLength,
                                 expected) != 0 ||
      !constantTimeEqual(expected, supplied, sizeof(expected))) {
    LOGF("auth: owner MAC rejected\n");
    return false;
  }

  rememberOwnerSeq(seq);
  *body = bodyStart;
  *bodyLen = bodyLength;
  return true;
}

bool authVerify(const char *payload, size_t len, const char **body,
                size_t *bodyLen) {
  // --- split into the four lines without copying ---
  const char *p0 = payload;
  const char *end = payload + len;

  const char *n1 = (const char *)memchr(p0, '\n', end - p0);
  if (!n1) return false;
  if ((n1 - p0) != 2 || strncmp(p0, "v1", 2) != 0) {
    LOGF("auth: rejected - bad envelope version\n");
    return false;
  }

  const char *seqStart = n1 + 1;
  const char *n2 = (const char *)memchr(seqStart, '\n', end - seqStart);
  if (!n2) return false;

  const char *sigStart = n2 + 1;
  const char *n3 = (const char *)memchr(sigStart, '\n', end - sigStart);
  if (!n3) return false;

  const char *bodyStart = n3 + 1;
  const size_t bodyLength = (size_t)(end - bodyStart);

  // --- replay check before spending time on the maths ---
  const uint32_t seq = (uint32_t)strtoul(seqStart, nullptr, 10);
  if (seq <= lastSeq) {
    LOGF("auth: rejected - replay (seq %lu <= %lu)\n", (unsigned long)seq,
         (unsigned long)lastSeq);
    return false;
  }

  // --- decode the signature ---
  uint8_t sig[96];
  size_t sigLen = 0;
  if (mbedtls_base64_decode(sig, sizeof(sig), &sigLen,
                            (const uint8_t *)sigStart,
                            (size_t)(n3 - sigStart)) != 0) {
    LOGF("auth: rejected - signature not base64\n");
    return false;
  }

  // --- hash exactly what the signer hashed ---
  // "v1\n<seq>\n<body>": the payload with the signature line removed. Built by
  // hashing the two spans either side of it rather than reassembling a buffer,
  // so a long command cannot overflow anything here.
  uint8_t hash[32];
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  mbedtls_sha256_update(&sha, (const uint8_t *)p0, (size_t)(n2 - p0 + 1));
  mbedtls_sha256_update(&sha, (const uint8_t *)bodyStart, bodyLength);
  mbedtls_sha256_finish(&sha, hash);
  mbedtls_sha256_free(&sha);

  // --- verify against the one key we trust ---
  mbedtls_ecdsa_context ctx;
  mbedtls_ecdsa_init(&ctx);
  bool ok = false;
  if (mbedtls_ecp_group_load(&ctx.MBEDTLS_PRIVATE(grp),
                             MBEDTLS_ECP_DP_SECP256R1) == 0 &&
      mbedtls_ecp_point_read_binary(&ctx.MBEDTLS_PRIVATE(grp),
                                    &ctx.MBEDTLS_PRIVATE(Q), OPERATOR_PUB,
                                    OPERATOR_PUB_LEN) == 0) {
    ok = mbedtls_ecdsa_read_signature(&ctx, hash, sizeof(hash), sig, sigLen) == 0;
  }
  mbedtls_ecdsa_free(&ctx);

  if (!ok) {
    LOGF("auth: rejected - bad signature\n");
    return false;
  }

  // Only advance the counter once the signature is good, or an attacker could
  // burn the sequence space with garbage and lock out the real operator.
  rememberSeq(seq);
  *body = bodyStart;
  *bodyLen = bodyLength;
  return true;
}
