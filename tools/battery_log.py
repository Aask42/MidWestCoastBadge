#!/usr/bin/env python3 -u
"""Log badge runtime telemetry and report how long a set of batteries lasted.

Subscribes to <topic>/badge/+/telemetry, appends every sample to a CSV, and
prints a live table. When a badge stops reporting for longer than --dead-after,
it is declared dead and its LAST uptime is the answer you were after: how long
that pack ran the backlight and the WiFi radio.

That last-sample-before-silence approach is deliberate. A badge dying on
battery cannot send a farewell - the regulator drops out mid-frame - so the
runtime has to be inferred from when it went quiet, not from anything it says.

Usage
-----
  battery_log.py                                   # localhost broker
  battery_log.py --host 192.168.0.241 --csv run.csv
  battery_log.py --host xxx.hivemq.cloud --port 8883 --tls \\
                 --user badge --password secret

Leave it running for the whole discharge. Ctrl-C prints the summary.
"""

import argparse
import csv
import json
import os
import ssl
import sys
import time

try:
    import paho.mqtt.client as mqtt
except ImportError:
    sys.exit("paho-mqtt is required:  pip3 install paho-mqtt")


def hms(sec):
    sec = int(sec)
    return f"{sec // 3600}h{(sec % 3600) // 60:02d}m{sec % 60:02d}s"


class Tracker:
    def __init__(self, csv_path, dead_after):
        self.badges = {}          # id -> dict of last sample
        self.dead_after = dead_after
        self.csv_path = csv_path
        self.started = time.time()

        new = not os.path.exists(csv_path)
        self.fh = open(csv_path, "a", newline="")
        self.w = csv.writer(self.fh)
        if new:
            self.w.writerow(["wall_iso", "wall_epoch", "badge", "uptime_s",
                             "mv", "pct", "rssi", "heap", "show", "bright_pct"])
            self.fh.flush()

    def sample(self, d):
        bid = d.get("id", "?")
        now = time.time()
        prev = self.badges.get(bid)

        # A badge whose uptime went backwards rebooted. That matters: it means
        # the run you were timing ended, and a brownout reset looks exactly
        # like this. Call it out rather than silently continuing the tally.
        if prev and d.get("up", 0) < prev.get("up", 0):
            print(f"  ** {bid} REBOOTED (uptime {hms(prev['up'])} -> "
                  f"{hms(d.get('up', 0))}) - possible brownout **")

        d["_seen"] = now
        d["_first"] = prev["_first"] if prev else now
        self.badges[bid] = d

        self.w.writerow([time.strftime("%Y-%m-%dT%H:%M:%S"), f"{now:.0f}", bid,
                         d.get("up", ""), d.get("mv", ""), d.get("pct", ""),
                         d.get("rssi", ""), d.get("heap", ""),
                         d.get("show", ""), d.get("bright", "")])
        self.fh.flush()

    def table(self):
        now = time.time()
        rows = []
        for bid, d in sorted(self.badges.items()):
            quiet = now - d["_seen"]
            dead = quiet > self.dead_after
            pct = d.get("pct", -1)
            batt = f"{pct}%" if pct is not None and pct >= 0 else "no sense"
            mv = d.get("mv", 0)
            rows.append((bid, hms(d.get("up", 0)),
                         f"{mv}mV" if mv else "-", batt,
                         f"{d.get('rssi','?')}dBm", d.get("show", "?"),
                         "DEAD" if dead else f"{int(quiet)}s ago"))
        return rows

    def summary(self):
        print("\n=== runtime summary ===")
        if not self.badges:
            print("no telemetry received")
            return
        for bid, d in sorted(self.badges.items()):
            quiet = time.time() - d["_seen"]
            state = "died" if quiet > self.dead_after else "still running"
            print(f"{bid}: {state} after {hms(d.get('up', 0))} "
                  f"(last {d.get('mv', 0)}mV, {d.get('pct', -1)}%, "
                  f"backlight {d.get('bright','?')}%)")
        print(f"\nCSV: {self.csv_path}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--topic", default="dc34")
    ap.add_argument("--tls", action="store_true", help="required for HiveMQ")
    ap.add_argument("--user")
    ap.add_argument("--password")
    ap.add_argument("--csv", default="battery_run.csv")
    ap.add_argument("--dead-after", type=float, default=120.0,
                    help="seconds of silence before a badge counts as dead. "
                         "Must exceed the firmware's TELEMETRY_MS (30s) with "
                         "room for a missed sample or two.")
    a = ap.parse_args()

    t = Tracker(a.csv, a.dead_after)
    topic = f"{a.topic}/badge/+/telemetry"

    def on_connect(c, u, f, rc, props=None):
        print(f"connected to {a.host}:{a.port}, subscribing {topic}", flush=True)
        c.subscribe(topic)

    def on_message(c, u, msg):
        try:
            t.sample(json.loads(msg.payload.decode()))
        except (ValueError, UnicodeDecodeError):
            print(f"  (unparseable payload on {msg.topic})")

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

    client.connect(a.host, a.port, 60)
    client.loop_start()

    hdr = f"{'badge':<10} {'uptime':<12} {'volts':<9} {'batt':<10} {'rssi':<8} {'showing':<18} last"
    try:
        while True:
            time.sleep(10)
            rows = t.table()
            if rows:
                print(f"\n[{time.strftime('%H:%M:%S')}] "
                      f"logging {hms(time.time() - t.started)}", flush=True)
                print(hdr, flush=True)
                for r in rows:
                    print(f"{r[0]:<10} {r[1]:<12} {r[2]:<9} {r[3]:<10} "
                          f"{r[4]:<8} {r[5]:<18} {r[6]}", flush=True)
    except KeyboardInterrupt:
        t.summary()
    finally:
        client.loop_stop()


if __name__ == "__main__":
    main()
