# Badge Pairing & the Web Client

Companion to [MVP.md](MVP.md). The MVP gets images and a nametag onto a badge.
This document covers the next thing: letting someone control **their** badge
from a web page, without ever learning that MQTT is involved, and without any
other badge being able to impersonate them.

**Status:** design. The firmware ships the pairing secret and the keypair
(`identity.h`); the MQTT client and the web app are not built yet.

---

## What already exists on the badge

| Thing | Where | Notes |
|---|---|---|
| P-256 keypair | `identity.cpp`, NVS namespace `identity` | Generated once on first boot. Private half never leaves the device. Survives a factory reset — a reset clears your name and network, it does not make the badge a different badge. |
| Badge ID | derived | `SHA-256(pubkey)[0:4]` as 8 hex chars. Cannot be chosen independently of the key: two badges showing the same ID would have to share a keypair. |
| Pairing secret | `identity.cpp` | 5 chars, Crockford base32. Shown on the MQTT screen. Tapping the row rolls a new one. |

Verified generating on hardware — a fresh badge came up with ID/secret pair
and both `pub` and `priv` persisted in NVS.

### Why Crockford base32

No `I`, `L`, `O` or `U`. A code read aloud across a loud room, or squinted at
off a 240px panel, cannot be transcribed into a *different valid code*. That
property is worth more here than the four extra symbols would be.

---

## The thing this design has to get right

The obvious approach — badge subscribes to `badge/<id>/cmd`, web page publishes
to it — is wrong, and it is worth being explicit about why:

**Anyone who can reach the broker can publish to any topic.** MQTT ACLs help
only if every client has its own broker credentials, which is impossible here:
the web page is public and its credentials would ship to every visitor. Topic
names are not secrets, and neither is anything the broker can see.

So authentication has to happen **in the message**, not in the transport. That
is what the keypair is for, and it is the reason this cannot just be a shared
password.

---

## Threat model

| Attacker can | Assumed |
|---|---|
| Connect to the broker, subscribe to `#`, publish anywhere | **Yes** |
| Read every byte the broker relays | **Yes** |
| Precompute hashes offline at high speed | **Yes** |
| Physically read a badge's screen | Only if present — this is the trust anchor |
| Extract a private key from a badge's flash | Out of scope (physical access wins) |

The whole design rests on one assumption: **seeing the badge's screen means you
are entitled to control it.** The 5-char secret is the proof of that, and
nothing else.

---

## Be honest about 5 characters

32⁵ ≈ **33.5 million** ≈ 25 bits. That is *not* a cryptographic secret.

- Online guessing: fine. A badge accepting one claim attempt per second, only
  while unclaimed, would take over a year to brute force. Rate limiting handles
  this completely.
- Offline attack: **33.5M plain SHA-256 is milliseconds.** Anything that puts a
  fast hash of the secret on the wire is broken on arrival.

Three things make 25 bits sufficient anyway, and all three are required:

1. **A slow KDF.** PBKDF2-HMAC-SHA256, 100k iterations, over the secret. On the
   C3 that is ~200ms — irrelevant for a one-shot claim, and it turns the offline
   search into ~months of CPU rather than seconds.
2. **A short window.** The secret only matters while the badge is unclaimed.
   Once claimed, the badge stops listening on the claim topic entirely.
3. **Rotation.** Tapping the secret row issues a new code and invalidates any
   pairing in flight. That is the recovery path for reading your code out loud
   by mistake, and it is why it is one deliberate tap.

**The secret authenticates first contact. The keypair authenticates everything
after.** Get this distinction wrong and the design collapses back to a shared
password.

---

## Protocol

### Naming

```
dc34/claim/<claim-id>        claim rendezvous, derived from the secret
dc34/badge/<badge-id>/cmd    commands to a claimed badge (sealed)
dc34/badge/<badge-id>/state  badge -> world status (sealed)
```

### Deriving the claim topic

Rather than publishing a commitment to the secret and letting the client match
against it — which hands an attacker a free offline oracle — **the topic itself
is derived from the secret**, and both sides compute it independently:

```
K       = PBKDF2-HMAC-SHA256(secret, salt="dc34-claim-v1", iters=100000, 32 bytes)
claimId = base32(SHA-256("topic" || K)[0:8])
Kclaim  = HKDF(K, info="claim-aead")
```

Nothing derived from the secret is ever transmitted in a form that is cheap to
invert. An attacker subscribing to `dc34/claim/#` learns which claim topics are
*in use*, but mapping one back to a secret costs a full PBKDF2 pass per guess.

### Claim exchange

```
badge                                            web client
  |                                                   |
  |  subscribe dc34/claim/<claimId>                   |
  |  (only while unclaimed)                           |
  |                                                   |
  |            user reads "N3NG2" off the screen ---->|
  |                                                   |  derive claimId, Kclaim
  |                                                   |  generate ephemeral P-256
  |<-- AES-GCM(Kclaim, {clientPub, nonce}) -----------|
  |                                                   |
  |  decrypts => sender had the secret                |
  |  => physical possession proven                    |
  |                                                   |
  |--- AES-GCM(Kclaim, {badgePub, badgeId, sig}) ---->|
  |    sig = ECDSA(badgePriv, nonce)                  |
  |                                                   |  verify sig against badgePub
  |                                                   |
  |  ECDH(badgePriv, clientPub) ==> session key ==> ECDH(clientPriv, badgePub)
  |  store clientPub as authorised controller         |
  |  unsubscribe from claim topic                     |
```

The signature over the client's nonce is what stops a broker-side attacker from
replaying a captured claim response from a *different* badge. Without it, the
client would accept any `badgePub` that arrived on the right topic.

### Steady state

Every command is AES-GCM sealed under the ECDH session key, with a monotonic
counter in the AAD:

```json
{ "seq": 42, "op": "setName", "value": "aask42" }
```

- Badge drops anything that does not decrypt, and anything whose `seq` is not
  greater than the last accepted one. That is replay protection.
- The broker sees ciphertext only. It cannot forge, alter or replay a command.
- **"Each badge knows who is whose"** falls out of this: a badge only accepts
  commands sealed under a session key it established with one specific client
  public key. Another badge — or another visitor to the site — has a different
  key and its messages simply fail to open.

### Multiple controllers

Store an authorised-controller list, not a single key (phone *and* laptop is the
obvious case). Cap it, and put "forget all controllers" on the SETTINGS menu
next to factory reset.

---

## The web client

**Hard requirement: the user must never see the letters M, Q, T or T.** No
"broker", no "topic", no "connection". The MQTT client is a transport detail
that lives below the UI, in the same way that nobody's mail app says "SMTP".

| The user sees | What actually happens |
|---|---|
| "Enter the code on your badge" | PBKDF2 → derive claimId → subscribe |
| "Looking for your badge…" | MQTT-over-WebSockets connect, claim publish |
| "Found it! This is your badge." | signature verified, ECDH session established |
| "Set your name" | sealed `setName` command published |
| "Saved" | badge's sealed state update received |

### Shape

Static page. No backend — a backend would have to be trusted, and the whole
point is that it does not need to be.

```
web/
  index.html      the whole UI, three screens: code entry, connecting, control
  crypto.js       PBKDF2 / HKDF / ECDH / AES-GCM via WebCrypto only
  transport.js    MQTT over WebSockets. The ONLY file that knows about MQTT.
  app.js          UI state machine. Talks to transport.js through a queue of
                  {op, value} objects and never sees a topic string.
```

Keeping `transport.js` as the sole owner of every topic string is what makes
the "user doesn't know it's MQTT" requirement enforceable rather than
aspirational — swapping in WebTransport or a plain WebSocket relay later should
touch exactly one file.

### WebCrypto notes

- P-256 (`ECDH` + `ECDSA`) and AES-GCM are all natively available. No libraries.
- PBKDF2 is available but **100k iterations blocks the main thread** for a
  noticeable beat — run it in a worker, or the "Looking for your badge…" screen
  will jank on the first frame.
- The browser must reach the broker over **WSS**, not TCP. The broker needs a
  WebSocket listener and a real certificate; a self-signed one will be refused
  without an explicit user exception, which is a terrible first-run experience.

---

## Firmware work still required

Roughly in order:

1. **MQTT client.** Nothing connects today; `iotOnline` is hardcoded false and
   the status row reads "no broker" once WiFi is up.
2. **PBKDF2 + AES-GCM + ECDH** using mbedtls. All three are already linked in —
   `identity.cpp` uses the same library for keygen, so this is wiring, not new
   dependencies.
3. **Authorised-controller storage** in NVS, plus the `seq` counter (which must
   persist, or a reboot re-opens the replay window).
4. **Claim state machine**, including *stop listening once claimed*.

### Two known constraints

**The MQTT menu is full.** Seven rows already reach y=280 against a nav bar at
284. There is no room for an eighth, and a **broker password field is still
missing** — needed for any broker that isn't wide open. That needs either a
paged menu, a submenu, or moving broker auth into the claim flow entirely.

**Broker credentials are a separate problem from badge identity.** Everything
above authenticates *messages*. It says nothing about who may connect to the
broker at all. For a con, an open broker plus sealed payloads is probably the
right trade — but that should be a decision, not an accident.
