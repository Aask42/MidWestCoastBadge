// authcmd.cpp - see authcmd.h.

#include "authcmd.h"

#include <Preferences.h>
#include <mbedtls/base64.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>
#include <mbedtls/sha256.h>

#include "config.h"
#include "operator_pub.h"

// Highest sequence number accepted so far. Persisted, because a badge that
// forgot it across a reboot would accept a replay of anything captured before
// that reboot.
static uint32_t lastSeq = 0;

void authBegin() {
  Preferences p;
  p.begin("auth", true);
  lastSeq = p.getULong("seq", 0);
  p.end();
  LOGF("auth: trusting key %s, last seq %lu\n", OPERATOR_FINGERPRINT,
       (unsigned long)lastSeq);
}

const char *authFingerprint() { return OPERATOR_FINGERPRINT; }

static void rememberSeq(uint32_t seq) {
  lastSeq = seq;
  Preferences p;
  p.begin("auth", false);
  p.putULong("seq", seq);
  p.end();
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
