#!/usr/bin/env python3 -u
"""Push a firmware image to badges over the air.

Serves the .bin over HTTP from this machine, publishes an MQTT trigger, and
watches for each badge to come back on the new version.

Usage
-----
  # build first, then:
  ota_push.py --bin build/badge.ino.bin --broker 192.168.0.241

  # one badge only
  ota_push.py --bin f.bin --broker 192.168.0.241 --badge 68cd2517

  # HiveMQ
  ota_push.py --bin f.bin --broker x.hivemq.cloud --port 8883 --tls \\
              --user badge --password secret

The badge pulls over plain HTTP from this host, so this machine must be
reachable on the badge's network - which is why --http-host is auto-detected
rather than assumed to be localhost.
"""

import argparse
import json
import os
import ssl
import sys
import time

import badgeauth
import otaserve

try:
    import paho.mqtt.client as mqtt
except ImportError:
    sys.exit("paho-mqtt is required:  pip3 install paho-mqtt")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True, help="firmware .bin to push")
    ap.add_argument("--broker", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--topic", default="dc34")
    ap.add_argument("--badge", help="badge id; default is every badge")
    ap.add_argument("--tls", action="store_true")
    ap.add_argument("--user")
    ap.add_argument("--password")
    ap.add_argument("--http-port", type=int, default=8099)
    ap.add_argument("--http-host", help="override the auto-detected LAN IP")
    ap.add_argument("--no-md5", action="store_true",
                    help="skip integrity verification (not recommended)")
    ap.add_argument("--wait", type=float, default=120.0)
    ap.add_argument("--key", default=None, help="operator private key")
    a = ap.parse_args()

    path = os.path.abspath(a.bin)
    try:
        size, md5 = otaserve.inspect(path)
    except otaserve.BadImage as e:
        sys.exit(str(e))

    host = a.http_host or otaserve.lan_ip(a.broker)
    httpd = otaserve.Server(os.path.dirname(path), a.http_port)
    url = f"http://{host}:{httpd.port}/{os.path.basename(path)}"

    print(f"firmware : {path}")
    print(f"size     : {size} bytes ({size/1024:.0f} KB)")
    print(f"md5      : {md5}")
    print(f"serving  : {url}")

    seen_before = {}
    updated = set()
    started = set()   # badges that have acknowledged the trigger

    def on_connect(c, u, f, rc, props=None):
        c.subscribe(f"{a.topic}/badge/+/state")
        c.subscribe(f"{a.topic}/badge/+/telemetry")

    def on_message(c, u, msg):
        try:
            d = json.loads(msg.payload.decode())
        except (ValueError, UnicodeDecodeError):
            return
        bid = d.get("id")
        if not bid:
            return
        if "ota" in d:
            print(f"  {bid}: ota {d['ota']}"
                  + (f" - {d.get('err','')}" if d.get("ota") == "failed" else ""))
            if d["ota"] == "starting":
                started.add(bid)
            return

        v = d.get("v")
        if not v:
            return

        # The state doc published DURING an update carries {"ota":"starting"}
        # and no version, so version-vs-version comparison silently never
        # fires. Use the acknowledgement as the boundary instead: once a badge
        # has said "starting", the next state doc carrying a version is it
        # coming back up, and that is what confirms the install.
        if bid in started and bid not in updated:
            updated.add(bid)
            was = seen_before.get(bid, "?")
            print(f"  {bid}: back up on v{v}" +
                  (f" (was v{was})" if was != "?" else ""))
        elif bid not in seen_before:
            seen_before[bid] = v

    try:
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    except AttributeError:
        client = mqtt.Client()
    client.on_connect = on_connect
    client.on_message = on_message
    if a.user:
        client.username_pw_set(a.user, a.password or "")
    if a.tls:
        client.tls_set(cert_reqs=ssl.CERT_REQUIRED)
    client.connect(a.broker, a.port, 60)
    client.loop_start()

    # Let retained state arrive so we know which badges exist and what they are
    # running before anything is triggered.
    time.sleep(2)
    targets = [a.badge] if a.badge else sorted(seen_before)
    if not targets:
        print("\nno badges seen on the broker - are they online?")
        return
    print(f"\ntargets  : {', '.join(targets)}")

    payload = {"ota": url}
    if not a.no_md5:
        payload["md5"] = md5
    key = badgeauth.load_private(a.key)
    for bid in targets:
        # Signed per badge so each gets its own sequence number; a badge that
        # was offline cannot later replay another badge's trigger.
        client.publish(f"{a.topic}/badge/{bid}/cmd",
                       badgeauth.sign_command(payload, key))
        print(f"  -> triggered {bid} (signed)")

    print(f"\nwaiting up to {a.wait:.0f}s for badges to come back...")
    t0 = time.time()
    while time.time() - t0 < a.wait and len(updated) < len(targets):
        time.sleep(1)

    print()
    for bid in targets:
        print(f"{bid}: {'UPDATED' if bid in updated else 'no confirmation'}")
    httpd.shutdown()
    client.loop_stop()


if __name__ == "__main__":
    main()
