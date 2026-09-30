const encoder = new TextEncoder();

function bytesToBase64(bytes) {
  let binary = "";
  for (const byte of bytes) binary += String.fromCharCode(byte);
  return btoa(binary);
}

export async function deriveOwnerKey(code, badgeId) {
  const keyMaterial = await crypto.subtle.importKey(
    "raw",
    encoder.encode(code.toUpperCase()),
    "PBKDF2",
    false,
    ["deriveKey"],
  );
  return crypto.subtle.deriveKey(
    {
      name: "PBKDF2",
      hash: "SHA-256",
      salt: encoder.encode(`dc34-owner-v1:${badgeId.toLowerCase()}`),
      iterations: 100000,
    },
    keyMaterial,
    { name: "HMAC", hash: "SHA-256", length: 256 },
    false,
    ["sign"],
  );
}

function nextSequence(badgeId) {
  const storageKey = `dc34-owner-seq:${badgeId}`;
  const previous = Number.parseInt(localStorage.getItem(storageKey) || "0", 10);
  const now = Math.floor(Date.now() / 1000);
  const sequence = Math.max(now, previous + 1);
  if (sequence > 0xffffffff) throw new Error("Owner sequence exhausted");
  localStorage.setItem(storageKey, String(sequence));
  return sequence;
}

export async function sealOwnerCommand(key, badgeId, command) {
  const body = JSON.stringify(command);
  const sequence = nextSequence(badgeId);
  const signedText = `o1\n${sequence}\n${body}`;
  const mac = await crypto.subtle.sign("HMAC", key, encoder.encode(signedText));
  return `o1\n${sequence}\n${bytesToBase64(new Uint8Array(mac))}\n${body}`;
}
