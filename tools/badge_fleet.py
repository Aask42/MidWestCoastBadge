#!/usr/bin/env python3 -u
"""Fleet window: every badge on the broker, live, with maintenance controls.

    ./tools/badge_fleet.py --broker 192.168.0.241
    ./tools/badge_fleet.py --broker x.hivemq.cloud --port 8883 --tls \\
                           --user badge --password secret

The table fills itself from the retained `state` docs and the 30-second
`telemetry` feed. Select a badge to send it a banner, rename it, change what it
is showing, or push firmware to it alone. `Update all online` pushes to every
badge currently answering.

Every command leaves here signed with the operator key, exactly as
`badge_cmd.py` and `ota_push.py` send them - a badge drops anything unsigned,
so there is no unauthenticated path into this window.

Firmware is served over plain HTTP from this machine for the duration of an
update, so the badges have to be able to reach it; the address offered is the
one that routes toward the broker, on the assumption that whatever reaches the
broker reaches this host too.
"""

import argparse
import json
import os
import queue
import ssl
import sys
import threading
import time
import tkinter as tk
from tkinter import filedialog, ttk

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import badgeauth
import otaserve

try:
    import paho.mqtt.client as mqtt
except ImportError:
    sys.exit("paho-mqtt is required:  pip3 install paho-mqtt")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BIN = os.path.join(ROOT, ".build", "arduino", "badge.ino.bin")

# Telemetry arrives every 30s. One missed report is a hiccup; two means the
# badge is not there any more, and the row greys out rather than disappearing -
# a badge that dropped off is exactly what an operator wants to still see.
STALE_AFTER = 75
GONE_AFTER = 300

COLUMNS = [
    ("id", "Badge", 90),
    ("name", "Name", 130),
    ("v", "Ver", 55),
    ("show", "Showing", 110),
    ("batt", "Batt", 80),
    ("rssi", "RSSI", 60),
    ("up", "Uptime", 80),
    ("heap", "Heap", 70),
    ("seen", "Seen", 60),
    ("status", "Status", 110),
]


def human_uptime(secs):
    try:
        secs = int(secs)
    except (TypeError, ValueError):
        return ""
    d, rem = divmod(secs, 86400)
    h, rem = divmod(rem, 3600)
    m, _ = divmod(rem, 60)
    if d:
        return f"{d}d{h}h"
    if h:
        return f"{h}h{m:02d}m"
    return f"{m}m"


class Badge:
    """Everything known about one badge, merged from both of its topics."""

    def __init__(self, bid):
        self.id = bid
        self.name = ""
        self.version = ""
        self.show = ""
        self.mv = None
        self.pct = None
        self.rssi = None
        self.heap = None
        self.uptime = None
        self.last_seen = 0.0
        self.present = True   # cleared by the LWT
        self.ota = ""         # transient: starting / failed / updated

    @property
    def age(self):
        return time.time() - self.last_seen if self.last_seen else 1e9

    def status(self):
        if self.ota:
            return self.ota
        if not self.present:
            return "offline"
        if self.age > GONE_AFTER:
            return "gone"
        if self.age > STALE_AFTER:
            return "stale"
        return "online"

    def online(self):
        return self.present and self.age <= STALE_AFTER

    def row(self):
        batt = ""
        if self.pct is not None and self.pct >= 0:
            batt = f"{self.pct}%"
            if self.mv:
                batt += f" ({self.mv}mV)"
        elif self.mv:
            batt = f"{self.mv}mV"
        return (self.id, self.name, self.version, self.show, batt,
                "" if self.rssi is None else f"{self.rssi}",
                human_uptime(self.uptime),
                "" if self.heap is None else f"{self.heap // 1024}K",
                f"{int(self.age)}s" if self.last_seen else "",
                self.status())


class Fleet:
    """MQTT side. Runs on paho's thread and posts events into a queue.

    Tkinter may only be touched from the thread that made the widgets, so
    nothing here calls into the UI directly - the window drains the queue on a
    timer instead.
    """

    def __init__(self, args, events):
        self.args = args
        self.events = events
        self.badges = {}
        self.lock = threading.Lock()
        self.key = None
        self.connected = False

        try:
            self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
        except AttributeError:
            self.client = mqtt.Client()
        self.client.on_connect = self._on_connect
        self.client.on_disconnect = self._on_disconnect
        self.client.on_message = self._on_message
        if args.user:
            self.client.username_pw_set(args.user, args.password or "")
        if args.tls:
            self.client.tls_set(cert_reqs=ssl.CERT_REQUIRED)

    def start(self):
        try:
            self.key = badgeauth.load_private(self.args.key)
        except SystemExit as e:
            self.log(str(e))
            return
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self):
        while True:
            try:
                self.client.connect(self.args.broker, self.args.port, 60)
                self.client.loop_forever()
            except Exception as e:
                self.connected = False
                self.events.put(("conn", False))
                self.log(f"broker: {e}; retrying in 5s")
                time.sleep(5)

    def log(self, msg):
        self.events.put(("log", msg))

    def _on_connect(self, c, u, f, rc, props=None):
        self.connected = True
        self.events.put(("conn", True))
        c.subscribe(f"{self.args.topic}/badge/+/state")
        c.subscribe(f"{self.args.topic}/badge/+/telemetry")
        self.log(f"connected to {self.args.broker}:{self.args.port}")

    def _on_disconnect(self, c, u, *a):
        self.connected = False
        self.events.put(("conn", False))

    def _on_message(self, c, u, msg):
        parts = msg.topic.split("/")
        leaf = parts[-1]
        tid = parts[-2] if len(parts) >= 2 else None

        # The badge's last will is an EMPTY retained payload on the state
        # topic, so zero bytes is not a malformed message - it is the broker
        # telling us that badge dropped its connection.
        if leaf == "state" and not msg.payload.strip():
            if tid:
                with self.lock:
                    b = self.badges.get(tid)
                    if b:
                        b.present = False
                        b.ota = ""
                self.events.put(("refresh", None))
            return

        try:
            d = json.loads(msg.payload.decode())
        except (ValueError, UnicodeDecodeError):
            return
        bid = d.get("id") or tid
        if not bid:
            return

        with self.lock:
            b = self.badges.setdefault(bid, Badge(bid))
            b.present = True
            b.last_seen = time.time()

            # An OTA state doc carries no version, so it must not be allowed to
            # blank the fields the table is showing. Only copy what is present.
            if "ota" in d:
                if d["ota"] == "starting":
                    b.ota = "updating..."
                    self.log(f"{bid}: OTA started")
                elif d["ota"] == "failed":
                    b.ota = "OTA FAILED"
                    self.log(f"{bid}: OTA failed - {d.get('err', '?')}")
                self.events.put(("refresh", None))
                return

            if d.get("v"):
                # A version arriving after an update announcement is the badge
                # coming back up on the far side of it. That transition is the
                # only reliable confirmation an install worked: the badge is
                # not reachable while it is writing flash, so there is nothing
                # to poll in between.
                if b.ota == "updating...":
                    self.log(f"{bid}: back up on v{d['v']}")
                b.ota = ""
                b.version = d["v"]
            for src, dst in (("name", "name"), ("show", "show")):
                if d.get(src) is not None:
                    setattr(b, dst, d[src])
            for src, dst in (("mv", "mv"), ("pct", "pct"), ("rssi", "rssi"),
                             ("heap", "heap"), ("up", "uptime")):
                if d.get(src) is not None:
                    setattr(b, dst, d[src])

        self.events.put(("refresh", None))

    def snapshot(self):
        with self.lock:
            return sorted(self.badges.values(), key=lambda b: b.id)

    def send(self, command, badge_id=None):
        """Sign and publish. `badge_id` None means the broadcast topic."""
        if not self.key:
            self.log("no operator key loaded - cannot send")
            return False
        if not self.connected:
            self.log("not connected to the broker")
            return False
        topic = (f"{self.args.topic}/badge/{badge_id}/cmd" if badge_id
                 else f"{self.args.topic}/all/cmd")
        try:
            self.client.publish(topic, badgeauth.sign_command(command,
                                                              self.key))
        except Exception as e:
            self.log(f"publish failed: {e}")
            return False
        self.log(f"-> {topic}  {json.dumps(command)[:70]}")
        return True


class Window:
    def __init__(self, root, fleet, args):
        self.root = root
        self.fleet = fleet
        self.args = args
        self.events = fleet.events
        self.httpd = None
        self.bin_path = tk.StringVar(
            value=args.bin if args.bin else
            (DEFAULT_BIN if os.path.exists(DEFAULT_BIN) else ""))

        root.title(f"DC34 badge fleet - {args.broker}")
        root.geometry("1120x680")
        root.minsize(880, 520)

        self._build_table()
        self._build_controls()
        self._build_log()

        self.root.after(120, self._drain)
        self.root.after(1000, self._tick)

    # === Layout ===

    def _build_table(self):
        frame = ttk.Frame(self.root, padding=(8, 8, 8, 4))
        frame.pack(fill="both", expand=True)

        bar = ttk.Frame(frame)
        bar.pack(fill="x", pady=(0, 4))
        self.conn_label = ttk.Label(bar, text="connecting...")
        self.conn_label.pack(side="left")
        self.count_label = ttk.Label(bar, text="")
        self.count_label.pack(side="right")

        cols = [c[0] for c in COLUMNS]
        self.tree = ttk.Treeview(frame, columns=cols, show="headings",
                                 selectmode="extended")
        for key, title, width in COLUMNS:
            self.tree.heading(key, text=title)
            self.tree.column(key, width=width, anchor="w",
                             stretch=(key == "name"))
        vsb = ttk.Scrollbar(frame, orient="vertical",
                            command=self.tree.yview)
        self.tree.configure(yscrollcommand=vsb.set)
        self.tree.pack(side="left", fill="both", expand=True)
        vsb.pack(side="right", fill="y")

        # Colour carries the same information as the Status column so a full
        # rack can be read at a glance without any of it being colour-only.
        self.tree.tag_configure("online", foreground="#0a7d28")
        self.tree.tag_configure("stale", foreground="#b07000")
        self.tree.tag_configure("offline", foreground="#999999")
        self.tree.tag_configure("busy", foreground="#0050c0")
        self.tree.tag_configure("bad", foreground="#c00000")
        self.tree.bind("<<TreeviewSelect>>", lambda e: self._sync_selection())

    def _build_controls(self):
        box = ttk.LabelFrame(self.root, text="Maintenance", padding=8)
        box.pack(fill="x", padx=8, pady=4)

        self.target = ttk.Label(box, text="no badge selected")
        self.target.grid(row=0, column=0, columnspan=6, sticky="w",
                         pady=(0, 6))

        ttk.Label(box, text="Banner").grid(row=1, column=0, sticky="w")
        self.msg = ttk.Entry(box, width=42)
        self.msg.grid(row=1, column=1, sticky="we", padx=4)
        self.secs = ttk.Entry(box, width=5)
        self.secs.insert(0, "20")
        self.secs.grid(row=1, column=2, padx=(0, 4))
        ttk.Button(box, text="Send", width=9,
                   command=self.send_banner).grid(row=1, column=3)
        ttk.Button(box, text="Send to all", width=12,
                   command=lambda: self.send_banner(True)
                   ).grid(row=1, column=4, padx=4)

        ttk.Label(box, text="Name").grid(row=2, column=0, sticky="w",
                                         pady=(4, 0))
        self.newname = ttk.Entry(box, width=42)
        self.newname.grid(row=2, column=1, sticky="we", padx=4, pady=(4, 0))
        ttk.Button(box, text="Apply", width=9,
                   command=self.send_name).grid(row=2, column=3, pady=(4, 0))

        ttk.Label(box, text="Show #").grid(row=3, column=0, sticky="w",
                                           pady=(4, 0))
        self.show = ttk.Entry(box, width=6)
        self.show.insert(0, "0")
        self.show.grid(row=3, column=1, sticky="w", padx=4, pady=(4, 0))
        ttk.Button(box, text="Apply", width=9,
                   command=self.send_show).grid(row=3, column=3, pady=(4, 0))

        ttk.Label(box, text="Raw JSON").grid(row=4, column=0, sticky="w",
                                             pady=(4, 0))
        self.raw = ttk.Entry(box, width=42)
        self.raw.grid(row=4, column=1, sticky="we", padx=4, pady=(4, 0))
        ttk.Button(box, text="Send", width=9,
                   command=self.send_raw).grid(row=4, column=3, pady=(4, 0))

        box.columnconfigure(1, weight=1)

        ota = ttk.LabelFrame(self.root, text="Firmware", padding=8)
        ota.pack(fill="x", padx=8, pady=4)
        ttk.Entry(ota, textvariable=self.bin_path).grid(
            row=0, column=0, sticky="we", padx=(0, 4))
        ttk.Button(ota, text="Browse", width=9,
                   command=self.browse).grid(row=0, column=1)
        ttk.Button(ota, text="Update selected", width=16,
                   command=lambda: self.push(False)).grid(
                       row=0, column=2, padx=4)
        self.all_btn = ttk.Button(ota, text="Update all online", width=17,
                                  command=lambda: self.push(True))
        self.all_btn.grid(row=0, column=3)
        ota.columnconfigure(0, weight=1)

    def _build_log(self):
        frame = ttk.Frame(self.root, padding=(8, 0, 8, 8))
        frame.pack(fill="both")
        self.log_box = tk.Text(frame, height=8, wrap="none")
        sb = ttk.Scrollbar(frame, orient="vertical",
                           command=self.log_box.yview)
        self.log_box.configure(yscrollcommand=sb.set, state="disabled")
        self.log_box.pack(side="left", fill="both", expand=True)
        sb.pack(side="right", fill="y")

    # === Event pump ===

    def _drain(self):
        dirty = False
        try:
            while True:
                kind, payload = self.events.get_nowait()
                if kind == "log":
                    self._append(payload)
                elif kind == "conn":
                    self.conn_label.configure(
                        text=(f"connected to {self.args.broker}:"
                              f"{self.args.port}" if payload
                              else "DISCONNECTED - retrying"))
                elif kind == "refresh":
                    dirty = True
        except queue.Empty:
            pass
        if dirty:
            self.redraw()
        self.root.after(120, self._drain)

    def _tick(self):
        """Repaint once a second so the age column and staleness stay true."""
        self.redraw()
        self.root.after(1000, self._tick)

    def _append(self, msg):
        self.log_box.configure(state="normal")
        self.log_box.insert("end", f"{time.strftime('%H:%M:%S')}  {msg}\n")
        self.log_box.see("end")
        self.log_box.configure(state="disabled")

    # === Table ===

    def redraw(self):
        badges = self.fleet.snapshot()
        keep = self.selected_ids()
        have = set(self.tree.get_children(""))

        for b in badges:
            status = b.status()
            tag = ("busy" if status == "updating..."
                   else "bad" if status == "OTA FAILED"
                   else "online" if status == "online"
                   else "stale" if status == "stale"
                   else "offline")
            if b.id in have:
                self.tree.item(b.id, values=b.row(), tags=(tag,))
            else:
                self.tree.insert("", "end", iid=b.id, values=b.row(),
                                 tags=(tag,))

        # Selection survives a repaint: an operator picking a badge and then
        # losing it a second later when telemetry lands would be unusable.
        alive = {b.id for b in badges}
        for gone in have - alive:
            self.tree.delete(gone)
        restore = [i for i in keep if i in alive]
        if restore and set(restore) != set(self.tree.selection()):
            self.tree.selection_set(restore)

        online = sum(1 for b in badges if b.online())
        self.count_label.configure(
            text=f"{online} online / {len(badges)} known")

    def selected_ids(self):
        return list(self.tree.selection())

    def _sync_selection(self):
        ids = self.selected_ids()
        if not ids:
            self.target.configure(text="no badge selected - "
                                       "commands go to ALL badges")
        elif len(ids) == 1:
            b = next((x for x in self.fleet.snapshot() if x.id == ids[0]),
                     None)
            self.target.configure(
                text=f"selected: {ids[0]}"
                     + (f"  \"{b.name}\"  v{b.version}" if b else ""))
            if b and b.name and not self.newname.get():
                self.newname.insert(0, b.name)
        else:
            self.target.configure(text=f"selected: {len(ids)} badges")

    # === Commands ===

    def _targets(self):
        """Selected badges, or None meaning 'use the broadcast topic'.

        Sending to everyone is a real operation, not a mistake, so an empty
        selection is not an error - but it does go out as one broadcast rather
        than N addressed messages, which is what the badges' `all/cmd`
        subscription is for.
        """
        ids = self.selected_ids()
        return ids or None

    def send_banner(self, broadcast=False):
        text = self.msg.get().strip()
        if not text:
            self._append("nothing to send - the banner is empty")
            return
        try:
            secs = max(1, min(600, int(self.secs.get())))
        except ValueError:
            secs = 20
        cmd = {"msg": text, "secs": secs}
        if broadcast or self._targets() is None:
            self.fleet.send(cmd, None)
        else:
            for bid in self._targets():
                self.fleet.send(cmd, bid)

    def send_name(self):
        name = self.newname.get().strip()
        if not name:
            return
        ids = self._targets()
        if ids is None:
            self._append("select a badge first - "
                         "renaming every badge at once is never intended")
            return
        for bid in ids:
            self.fleet.send({"setName": name}, bid)

    def send_show(self):
        try:
            n = int(self.show.get())
        except ValueError:
            self._append("show must be a number")
            return
        ids = self._targets()
        if ids is None:
            self.fleet.send({"setShow": n}, None)
        else:
            for bid in ids:
                self.fleet.send({"setShow": n}, bid)

    def send_raw(self):
        try:
            cmd = json.loads(self.raw.get())
        except ValueError as e:
            self._append(f"raw JSON is not valid: {e}")
            return
        ids = self._targets()
        if ids is None:
            self.fleet.send(cmd, None)
        else:
            for bid in ids:
                self.fleet.send(cmd, bid)

    def browse(self):
        p = filedialog.askopenfilename(
            title="Firmware image",
            initialdir=os.path.dirname(self.bin_path.get() or DEFAULT_BIN),
            filetypes=[("ESP32 image", "*.bin"), ("All files", "*")])
        if p:
            self.bin_path.set(p)

    def push(self, everyone):
        path = self.bin_path.get().strip()
        try:
            size, md5 = otaserve.inspect(path)
        except otaserve.BadImage as e:
            self._append(str(e))
            return

        badges = self.fleet.snapshot()
        if everyone:
            targets = [b.id for b in badges if b.online()]
        else:
            targets = self.selected_ids()
            offline = [b.id for b in badges
                       if b.id in targets and not b.online()]
            if offline:
                self._append(f"note: {', '.join(offline)} "
                             f"{'is' if len(offline) == 1 else 'are'} "
                             f"not answering; triggering anyway")
        if not targets:
            self._append("no badges to update")
            return

        # One server for the whole session, restarted only when the file
        # changes. Thirty badges pulling 1.3MB each is the load it has to
        # carry, so it is threaded and it stays up until the window closes.
        if self.httpd is None:
            try:
                self.httpd = otaserve.Server(os.path.dirname(
                    os.path.abspath(path)), self.args.http_port)
            except OSError as e:
                self._append(f"cannot serve firmware on port "
                             f"{self.args.http_port}: {e}")
                return
        host = self.args.http_host or otaserve.lan_ip(self.args.broker)
        url = f"http://{host}:{self.httpd.port}/{os.path.basename(path)}"

        self._append(f"serving {os.path.basename(path)} "
                     f"({size / 1024:.0f} KB, md5 {md5[:8]}) at {url}")

        payload = {"ota": url, "md5": md5}
        sent = 0
        for bid in targets:
            # Signed per badge, so each carries its own sequence number and a
            # badge that was offline cannot later replay another's trigger.
            if self.fleet.send(payload, bid):
                sent += 1
        self._append(f"triggered {sent} badge(s) - watch the Status column")

    def close(self):
        if self.httpd:
            self.httpd.shutdown()
        self.root.destroy()


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--broker", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--topic", default="dc34")
    ap.add_argument("--tls", action="store_true")
    ap.add_argument("--user")
    ap.add_argument("--password")
    ap.add_argument("--key", default=None, help="operator private key")
    ap.add_argument("--bin", help="firmware image to preload in the box")
    ap.add_argument("--http-port", type=int, default=8099)
    ap.add_argument("--http-host", help="override the auto-detected LAN IP")
    args = ap.parse_args()

    events = queue.Queue()
    fleet = Fleet(args, events)

    root = tk.Tk()
    win = Window(root, fleet, args)
    root.protocol("WM_DELETE_WINDOW", win.close)
    fleet.start()
    root.mainloop()


if __name__ == "__main__":
    main()
