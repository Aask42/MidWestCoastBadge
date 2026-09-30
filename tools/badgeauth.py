"""Shared signing helpers for the badge operator key.

The badges trust exactly one public key, baked into the firmware at build time.
Anything published to a badge's `cmd` topic - or to the broadcast topic that
every badge listens on - must carry a signature from the matching private key
or it is dropped unread.

Wire format, chosen so the badge can parse it without a JSON library and
without any escaping ambiguity:

    v1\\n
    <seq>\\n
    <base64 DER signature>\\n
    <command json>

The signature covers `v1\\n<seq>\\n<command json>` - everything except the
signature line itself. Keeping the payload as raw trailing bytes means the
badge never has to un-escape a JSON string inside another JSON string, which
is exactly the kind of parser subtlety that turns into a security bug.

`seq` is a monotonic counter, persisted on both sides. A badge rejects any
command whose seq is not greater than the last one it accepted, so a captured
message cannot be replayed later.
"""

import base64
import json
import os
import time

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.exceptions import InvalidSignature

WIRE_VERSION = "v1"


def default_key_path():
    return os.path.expanduser("~/.dc34_badge_operator.pem")


def default_seq_path():
    return os.path.expanduser("~/.dc34_badge_operator.seq")


def load_private(path=None):
    path = path or default_key_path()
    if not os.path.exists(path):
        raise SystemExit(
            f"no operator key at {path}\n"
            f"generate one with:  python3 tools/make_operator_key.py")
    with open(path, "rb") as f:
        return serialization.load_pem_private_key(f.read(), password=None)


def next_seq(path=None):
    """Monotonic counter, persisted so it survives restarts of the tooling.

    Seeded from the wall clock rather than from zero: if this file is ever
    lost, restarting at 0 would produce commands every badge rejects as
    replays until the counter climbed past whatever they last accepted.
    Seconds-since-epoch is always ahead of any previously issued value.
    """
    path = path or default_seq_path()
    seq = int(time.time())
    if os.path.exists(path):
        try:
            seq = max(seq, int(open(path).read().strip()) + 1)
        except ValueError:
            pass
    with open(path, "w") as f:
        f.write(str(seq))
    return seq


def sign_command(command: dict, key=None, seq=None) -> bytes:
    """Builds a signed wire message for `command`."""
    key = key or load_private()
    seq = seq if seq is not None else next_seq()
    body = json.dumps(command, separators=(",", ":"), sort_keys=True)
    signed = f"{WIRE_VERSION}\n{seq}\n{body}".encode()
    sig = key.sign(signed, ec.ECDSA(hashes.SHA256()))
    return (f"{WIRE_VERSION}\n{seq}\n"
            f"{base64.b64encode(sig).decode()}\n{body}").encode()


def verify_message(raw: bytes, pub) -> dict:
    """Host-side mirror of the badge's check, for testing the firmware path."""
    parts = raw.split(b"\n", 3)
    if len(parts) != 4 or parts[0].decode() != WIRE_VERSION:
        raise ValueError("bad envelope")
    seq, sig_b64, body = parts[1], parts[2], parts[3]
    signed = b"\n".join([parts[0], seq, body])
    try:
        pub.verify(base64.b64decode(sig_b64), signed,
                   ec.ECDSA(hashes.SHA256()))
    except InvalidSignature:
        raise ValueError("bad signature")
    return {"seq": int(seq), "command": json.loads(body)}
