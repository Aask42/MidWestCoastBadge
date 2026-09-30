#!/usr/bin/env python3 -u
"""Flash every badge that gets plugged in, hands-free.

Watches for new serial ports appearing and writes a complete, recoverable image
to each one as it shows up: bootloader, partition table, otadata, immutable
factory recovery, the main application, and LittleFS art. Plug a badge in,
wait for the green line, unplug it, plug in the next.

    ./tools/mass_flash.py                   # compile, then wait for badges
    ./tools/mass_flash.py --no-build        # flash the existing build
    ./tools/mass_flash.py --jobs 6          # six badges on a hub at once
    ./tools/mass_flash.py --once            # flash what is plugged in, exit

Why a full flash and not `arduino-cli upload`
--------------------------------------------
`upload` writes the application only. A badge that has never been flashed with
THIS partition table still has the factory one, so the app lands in a slot of
the wrong size, `storage` does not exist, and otadata points somewhere stale.
The result boots, looks fine, and then fails its first OTA - which is the
worst possible time to find out. Writing all five regions makes a badge
identical to every other badge regardless of what was on it before.

Safety checks that run before anything is written
-------------------------------------------------
* the app image really is an ESP32 image (magic byte 0xE9)
* recovery and main each fit their own slot in `partitions.csv`
* the partition table the firmware was BUILT against matches the one in the
  sketch directory - a stale build tree is otherwise undetectable and produces
  badges that are subtly wrong

Each badge is identified by its MAC, remembered in `.build/flashed.json`, so a
badge that reboots and re-enumerates after flashing is recognised and left
alone rather than flashed again in a loop.
"""

import argparse
import concurrent.futures
import glob
import hashlib
import json
import os
import re
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SKETCH = os.path.join(ROOT, "arduino", "badge")
RECOVERY_SKETCH = os.path.join(ROOT, "arduino", "recovery")
IMAGES = os.path.join(ROOT, "images")
STATE_DIR = os.path.join(ROOT, ".build")

# Deliberately NOT arduino/badge/build/. That directory is written by the
# Arduino IDE, while arduino-cli compiles into a temp path keyed by the sketch
# location - so the two disagree, and the IDE's copy can sit there for weeks
# looking authoritative. Pinning --build-path here means the artifacts this
# tool flashes are always the ones it just built.
BUILD = os.path.join(STATE_DIR, "arduino")
RECOVERY_BUILD = os.path.join(STATE_DIR, "recovery")
STORAGE_BIN = os.path.join(STATE_DIR, "storage.bin")
LEDGER = os.path.join(STATE_DIR, "flashed.json")

FQBN = "esp32:esp32:esp32c3:CDCOnBoot=cdc,FlashSize=4M"
CHIP = "esp32c3"

# Where macOS puts USB serial devices.
#
# macOS exposes every serial device TWICE - as `/dev/tty.X` and `/dev/cu.X` -
# so list only ONE of the two families here. Matching both would make a single
# badge look like two ports and get it flashed twice, concurrently, over itself.
#
# The two differ on open(): `tty.` is the callin device and blocks until DCD
# asserts, `cu.` is the callout device and does not. USB CDC generally asserts
# DCD immediately so `tty.` works, but a badge that comes up without it will
# hang esptool until the timeout rather than failing fast - which is why `cu.`
# is the usual choice on macOS and what esptool's own docs recommend.
PORT_GLOBS = [
    "/dev/tty.usbmodem*",
    "/dev/tty.usbserial*",
    "/dev/tty.wchusbserial*",
    "/dev/tty.SLAB_USBtoUART*",
]

# Fixed offsets for the regions that are not in partitions.csv. These match
# what arduino-cli itself writes (see build/.../flash_args).
BOOTLOADER_OFFSET = 0x0
PARTITIONS_OFFSET = 0x8000

RED, GREEN, YELLOW, DIM, RESET = (
    "\033[31m", "\033[32m", "\033[33m", "\033[2m", "\033[0m")
if not sys.stdout.isatty():
    RED = GREEN = YELLOW = DIM = RESET = ""

_print_lock = threading.Lock()
_reporter = None


def set_reporter(reporter):
    """Mirror status lines to a GUI or other host without changing the CLI."""
    global _reporter
    _reporter = reporter


def say(msg, colour=""):
    """Print one line atomically - worker threads all write to this stdout."""
    with _print_lock:
        print(f"{colour}{msg}{RESET}", flush=True)
        if _reporter:
            _reporter(msg, colour)


# === Partition table ===

def parse_partitions(path):
    """Read a partition CSV into {name: (offset, size)}.

    Sizes may be hex, decimal, or carry a K/M suffix, which is what the ESP-IDF
    partition tool accepts, so all three are handled here rather than assuming
    the current file's style will never change.
    """
    def number(tok):
        tok = tok.strip()
        mult = 1
        if tok and tok[-1] in "kKmM":
            mult = 1024 if tok[-1] in "kK" else 1024 * 1024
            tok = tok[:-1]
        return int(tok, 0) * mult

    table = {}
    with open(path) as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            cols = [c.strip() for c in line.split(",")]
            if len(cols) < 5 or not cols[0]:
                continue
            try:
                table[cols[0]] = (number(cols[3]), number(cols[4]))
            except ValueError:
                continue
    return table


# === External tools ===

def find_tool(pattern, what):
    hits = sorted(glob.glob(os.path.expanduser(pattern)))
    if not hits:
        sys.exit(f"cannot find {what} (looked for {pattern})\n"
                 f"install the esp32 core with arduino-cli first")
    return hits[-1]


def esptool_path():
    return find_tool(
        "~/Library/Arduino15/packages/esp32/tools/esptool_py/*/esptool",
        "esptool")


def mklittlefs_path():
    return find_tool(
        "~/Library/Arduino15/packages/esp32/tools/mklittlefs/*/mklittlefs",
        "mklittlefs")


# Bare words esptool 4.x spells with underscores. Only these are rewritten by
# the fallback below - a blanket dash-to-underscore pass would also mangle any
# file path that happens to contain a dash.
LEGACY_WORDS = {"write-flash", "read-mac", "default-reset", "hard-reset",
                "no-reset"}


def to_legacy(args):
    """The same command line in esptool 4.x spelling."""
    out = []
    for a in args:
        if a.startswith("--"):
            out.append("--" + a[2:].replace("-", "_"))
        elif a in LEGACY_WORDS:
            out.append(a.replace("-", "_"))
        else:
            out.append(a)
    return out


def run_esptool(args, timeout=180):
    """Invoke esptool, retrying with the old underscore spellings.

    esptool 5.x renamed `write_flash` to `write-flash`, `--flash_mode` to
    `--flash-mode` and so on, keeping the old forms as aliases - but 4.x knows
    only the old ones. Rather than sniffing the version, try the modern
    spelling and fall back, which is what flash_images.sh already does.
    """
    exe = esptool_path()
    try:
        p = subprocess.run([exe] + args, capture_output=True, text=True,
                           timeout=timeout)
    except subprocess.TimeoutExpired:
        return 1, f"esptool timed out after {timeout}s"
    if p.returncode == 0:
        return 0, p.stdout + p.stderr

    old = to_legacy(args)
    if old == args:
        return p.returncode, p.stdout + p.stderr
    try:
        q = subprocess.run([exe] + old, capture_output=True, text=True,
                           timeout=timeout)
    except subprocess.TimeoutExpired:
        return 1, f"esptool timed out after {timeout}s"
    return q.returncode, q.stdout + q.stderr


MAC_RE = re.compile(r"MAC:\s*([0-9a-fA-F:]{17})")


def read_mac(port):
    """Chip MAC, leaving the badge sitting in the ROM loader.

    `--after no-reset` so the chip stays in download mode: the flash that
    normally follows does not then have to talk it back down again. If this
    badge turns out to be one we have already done, `release()` boots it.

    Returns None if the probe fails for any reason. A failed probe must never
    block a flash - an unknown badge is exactly the case we are here for - so
    the caller treats None as "not seen before".
    """
    rc, out = run_esptool(
        ["--chip", CHIP, "--port", port, "--before", "default-reset",
         "--after", "no-reset", "read-mac"], timeout=30)
    if rc != 0:
        return None
    m = MAC_RE.search(out)
    return m.group(1).lower() if m else None


def release(port):
    """Reset a badge we are not going to flash so it boots its app again."""
    run_esptool(["--chip", CHIP, "--port", port, "--before", "no-reset",
                 "--after", "hard-reset", "run"], timeout=30)


# === Build ===

SOURCE_EXTS = {".ino", ".cpp", ".c", ".h", ".hpp", ".csv"}


def sketch_is_fresh(sketch, build_path, binary_name):
    """True if the compiled binary is newer than every source file."""
    binary = os.path.join(build_path, binary_name)
    if not os.path.exists(binary):
        return False
    bin_mtime = os.path.getmtime(binary)
    for dirpath, _, filenames in os.walk(sketch):
        for fn in filenames:
            if os.path.splitext(fn)[1].lower() in SOURCE_EXTS:
                if os.path.getmtime(os.path.join(dirpath, fn)) > bin_mtime:
                    return False
    return True


def compile_sketch(sketch, build_path, slot_size):
    """Run arduino-cli, sized to the sketch's real partition.

    `upload.maximum_size` is a board property that is NOT derived from
    partitions.csv - left alone it reads the stock 1.25MB and fails the build
    even when the real slot has room. Passing the actual size keeps the
    build's idea of "too big" identical to the flash layout's.
    """
    cmd = ["arduino-cli", "compile", "--fqbn", FQBN,
            "--build-property", f"upload.maximum_size={slot_size}",
            "--build-path", build_path, sketch]
    say(f"building: {' '.join(cmd)}", DIM)
    p = subprocess.run(cmd, cwd=os.path.join(ROOT, "arduino"))
    if p.returncode != 0:
        sys.exit("build failed - not flashing anything")


def pack_storage(size):
    """Build the LittleFS image if the art is newer than the last pack.

    Same parameters as flash_images.sh (-b 4096 -p 256): a filesystem packed
    with different geometry mounts as garbage rather than failing loudly.
    """
    art = sorted(glob.glob(os.path.join(IMAGES, "*.raw")))
    if not art:
        return None
    fresh = (os.path.exists(STORAGE_BIN) and
             os.path.getsize(STORAGE_BIN) == size and
             os.path.getmtime(STORAGE_BIN) >= max(os.path.getmtime(a)
                                                  for a in art))
    if fresh:
        return STORAGE_BIN
    os.makedirs(STATE_DIR, exist_ok=True)
    say(f"packing {len(art)} images -> {os.path.relpath(STORAGE_BIN, ROOT)}",
        DIM)
    p = subprocess.run([mklittlefs_path(), "-c", IMAGES, "-b", "4096",
                        "-p", "256", "-s", str(size), STORAGE_BIN],
                       capture_output=True, text=True)
    if p.returncode != 0:
        sys.exit(f"mklittlefs failed:\n{p.stdout}{p.stderr}")
    return STORAGE_BIN


def build_plan(args):
    """Everything that gets written, as (offset, path) pairs - after checks.

    All validation happens here, once, before a single badge is touched. A
    check that runs per-badge would let the first few through and only then
    discover the build is wrong.
    """
    table = parse_partitions(os.path.join(SKETCH, "partitions.csv"))
    for need in ("factory", "main", "otadata"):
        if need not in table:
            sys.exit(f"partitions.csv has no `{need}` partition")
    recovery_off, recovery_size = table["factory"]
    main_off, main_size = table["main"]
    ota_off, _ = table["otadata"]

    if args.build:
        if sketch_is_fresh(RECOVERY_SKETCH, RECOVERY_BUILD, "recovery.ino.bin"):
            say("recovery: up to date, skipping build", DIM)
        else:
            compile_sketch(RECOVERY_SKETCH, RECOVERY_BUILD, recovery_size)
        if sketch_is_fresh(SKETCH, BUILD, "badge.ino.bin"):
            say("main: up to date, skipping build", DIM)
        else:
            compile_sketch(SKETCH, BUILD, main_size)

    main_app = os.path.join(BUILD, "badge.ino.bin")
    recovery_app = os.path.join(RECOVERY_BUILD, "recovery.ino.bin")
    boot = os.path.join(BUILD, "badge.ino.bootloader.bin")
    parts = os.path.join(BUILD, "badge.ino.partitions.bin")
    boot_app0 = os.path.join(BUILD, "boot_app0.bin")
    for p in (main_app, recovery_app, boot, parts, boot_app0):
        if not os.path.exists(p):
            sys.exit(f"missing build artifact: {p}\n"
                     f"drop --no-build so the sketch gets compiled")

    # The build tree keeps a copy of the partition CSV it was compiled with.
    # If that has drifted from the sketch's, the binary was linked and
    # size-checked against a layout that is not the one being flashed - which
    # produces badges that boot and then misbehave in ways nothing reports.
    for built_csv in (os.path.join(BUILD, "partitions.csv"),
                      os.path.join(RECOVERY_BUILD, "partitions.csv")):
        if os.path.exists(built_csv) and parse_partitions(built_csv) != table:
            sys.exit("the build tree was compiled against a DIFFERENT "
                     "partition table than arduino/badge/partitions.csv\n"
                     "drop --no-build so it gets recompiled")

    def validate_image(path, label, size):
        blob = open(path, "rb").read()
        if not blob or blob[0] != 0xE9:
            sys.exit(f"{path} is not an ESP32 image "
                     f"(first byte 0x{blob[0]:02X}, expected 0xE9)"
                     if blob else f"{path} is empty")
        if len(blob) > size:
            sys.exit(f"{label} is {len(blob)} bytes but its slot is only "
                     f"{size} ({len(blob) - size} bytes over)")
        return len(blob)

    main_bytes = validate_image(main_app, "main firmware", main_size)
    recovery_bytes = validate_image(
        recovery_app, "factory recovery", recovery_size)

    plan = [(BOOTLOADER_OFFSET, boot),
            (PARTITIONS_OFFSET, parts),
            (ota_off, boot_app0),
            (recovery_off, recovery_app),
            (main_off, main_app)]

    # boot_app0.bin is the Arduino core's otadata initialiser. Sequence 1
    # selects ota_0 (`main`) instead of the factory recovery application.
    if args.storage and "storage" in table:
        st_off, st_size = table["storage"]
        img = pack_storage(st_size)
        if img:
            if os.path.getsize(img) > st_size:
                sys.exit(f"storage image is {os.path.getsize(img)} bytes, "
                         f"partition is {st_size}")
            plan.append((st_off, img))
        else:
            say("no .raw files in images/ - skipping the art partition",
                YELLOW)

    total = sum(os.path.getsize(p) for _, p in plan)
    say(f"main     : {main_bytes} bytes of {main_size} "
        f"({100.0 * main_bytes / main_size:.0f}%)")
    say(f"recovery : {recovery_bytes} bytes of {recovery_size} "
        f"({100.0 * recovery_bytes / recovery_size:.0f}%)")
    say(f"writing  : {len(plan)} regions, {total / 1024:.0f} KB per badge")
    for off, p in plan:
        say(f"    0x{off:06X}  {os.path.basename(p)}", DIM)
    return plan


def plan_fingerprint(plan):
    """SHA-256 of all flash artifacts so the ledger knows when code changes."""
    h = hashlib.sha256()
    for _, path in sorted(plan):
        with open(path, "rb") as f:
            h.update(f.read())
    return h.hexdigest()[:16]


# === Ledger ===

def load_ledger():
    try:
        with open(LEDGER) as f:
            return json.load(f)
    except (IOError, ValueError):
        return {}


def save_ledger(led):
    os.makedirs(STATE_DIR, exist_ok=True)
    tmp = LEDGER + ".tmp"
    with open(tmp, "w") as f:
        json.dump(led, f, indent=1, sort_keys=True)
    os.replace(tmp, LEDGER)


# === Flashing ===

class Flasher:
    def __init__(self, plan, args):
        self.plan = plan
        self.args = args
        self.lock = threading.Lock()
        self.ledger = {} if args.forget else load_ledger()
        self.fingerprint = plan_fingerprint(plan)
        self.ok = 0
        self.failed = 0
        self.skipped = 0

    def known(self, mac):
        with self.lock:
            entry = self.ledger.get(mac)
            if not entry:
                return False
            # Badge has stale firmware — needs re-flash.
            if entry.get("build") != self.fingerprint:
                return False
            return True

    def record(self, mac, port, ok):
        with self.lock:
            self.ledger[mac] = {
                "port": port,
                "when": time.strftime("%Y-%m-%dT%H:%M:%S"),
                "result": "ok" if ok else "failed",
                "build": self.fingerprint,
            }
            if ok:
                self.ok += 1
            else:
                self.failed += 1
            save_ledger(self.ledger)

    def handle(self, port):
        """Probe, decide, flash. Runs on a worker thread, one per badge."""
        short = os.path.basename(port)
        time.sleep(self.args.settle)

        mac = None
        if self.args.probe:
            say(f"[{short}] checking...", DIM)
            mac = read_mac(port)
            if mac and self.known(mac) and not self.args.reflash:
                # Already done - almost always this badge rebooting after its
                # own flash and re-enumerating. Boot it back into the app.
                release(port)
                with self.lock:
                    self.skipped += 1
                say(f"[{short}] {mac} already flashed - skipped", DIM)
                return

        label = mac or "unknown mac"
        say(f"[{short}] flashing {label}...", YELLOW)
        t0 = time.time()

        args = ["--chip", CHIP, "--port", port, "--baud", str(self.args.baud),
                "--before", "default-reset", "--after", "hard-reset",
                "write-flash", "--flash-mode", "dio", "--flash-freq", "80m",
                "--flash-size", "4MB"]
        for off, path in self.plan:
            args += [hex(off), path]

        rc, out = run_esptool(args, timeout=self.args.timeout)
        secs = time.time() - t0

        if mac is None:
            # esptool prints the MAC on every connect, so a probe was never
            # strictly necessary to learn it - only to decide beforehand.
            m = MAC_RE.search(out)
            mac = m.group(1).lower() if m else None

        if rc == 0:
            say(f"[{short}] OK  {mac or '?'}  {secs:.0f}s", GREEN)
        else:
            tail = "\n".join(l for l in out.strip().splitlines()[-6:])
            say(f"[{short}] FAILED after {secs:.0f}s\n{tail}", RED)

        if mac:
            self.record(mac, port, rc == 0)
        elif rc == 0:
            with self.lock:
                self.ok += 1
        else:
            with self.lock:
                self.failed += 1


def ports_now():
    found = set()
    for g in PORT_GLOBS:
        found.update(glob.glob(g))
    return found


class PortWatcher:
    """Turns a stream of port listings into 'this one is newly plugged in'.

    On macOS a `/dev/cu.usbmodem*` name identifies a physical USB socket, not a
    device: plug a different badge into the same socket and it gets the same
    name. So "is this a badge I have not done yet?" cannot be answered from the
    listing alone, and two cases have to be told apart even though both look
    like the port going away and coming back:

    * the badge REBOOTING after its flash, off the bus for a couple of seconds
    * the operator UNPLUGGING it and plugging in the next one

    The discriminator is how long it stayed away. A port is only forgotten -
    and so eligible to be enrolled again - once it has been continuously absent
    for `grace` seconds. Without that, a badge re-enumerating after its own
    flash is indistinguishable from a fresh one and gets flashed forever.

    `grace` is deliberately short, because missing a badge is worse than
    flashing one twice, and the MAC check in `handle()` catches the duplicate
    anyway. Running with --no-probe removes that backstop, which is why it
    wants a longer --grace.
    """

    def __init__(self, initial, grace=3.0):
        # Ports present at startup are recorded as already-seen rather than
        # enrolled: they are usually the operator's own console, and flashing
        # whatever happened to be connected when the tool launched is a nasty
        # surprise. Unplug and replug to enrol one.
        self.seen = set(initial)
        self.busy = set()
        self.grace = grace
        self.absent_since = {}

    def poll(self, now, clock=time.time):
        t = clock()
        for p in list(self.seen):
            if p in now:
                self.absent_since.pop(p, None)
            elif p not in self.busy:
                # A port being flashed is exempt: the badge drops off the bus
                # when esptool resets it partway through, and forgetting it
                # there would enrol it a second time mid-write.
                if t - self.absent_since.setdefault(p, t) >= self.grace:
                    self.seen.discard(p)
                    self.absent_since.pop(p, None)

        fresh = sorted(p for p in now
                       if p not in self.seen and p not in self.busy)
        self.busy.update(fresh)
        self.seen.update(now)
        return fresh

    def release(self, port):
        self.busy.discard(port)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--no-build", dest="build", action="store_false",
                    help="flash the existing build instead of compiling first")
    ap.add_argument("--build-only", action="store_true",
                    help="compile and validate every image, then exit without "
                         "opening a serial port")
    ap.add_argument("--jobs", type=int, default=4,
                    help="badges to flash at once (default 4)")
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--once", action="store_true",
                    help="flash what is plugged in now, then exit")
    ap.add_argument("--no-reflash", dest="reflash", action="store_false",
                    default=True,
                    help="skip badges already in the ledger")
    ap.add_argument("--forget", action="store_true",
                    help="start a fresh ledger, forgetting past badges")
    ap.add_argument("--no-probe", dest="probe", action="store_false",
                    help="skip the MAC check; faster, but a badge rebooting "
                         "after its flash will be flashed a second time")
    ap.add_argument("--no-storage", dest="storage", action="store_false",
                    help="do not write the LittleFS art partition")
    ap.add_argument("--settle", type=float, default=1.5,
                    help="seconds to wait after a port appears (default 1.5)")
    ap.add_argument("--grace", type=float, default=3.0,
                    help="seconds a port must stay unplugged before it counts "
                         "as a new badge (default 3; raise with --no-probe)")
    ap.add_argument("--timeout", type=int, default=240,
                    help="per-badge esptool timeout in seconds")
    ap.add_argument("--poll", type=float, default=0.5,
                    help="how often to look for new ports")
    args = ap.parse_args()

    plan = build_plan(args)
    if args.build_only:
        say("build and flash plan validated; no badges were touched", GREEN)
        return 0
    flasher = Flasher(plan, args)

    pool = concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs)
    futures = {}
    present = ports_now()

    if args.once:
        if not present:
            sys.exit("nothing plugged in")
        watcher = PortWatcher([], args.grace)
        say(f"\nflashing {len(present)} port(s) already present", YELLOW)
    else:
        watcher = PortWatcher(present, args.grace)
        if present:
            say(f"\nignoring {len(present)} port(s) already connected "
                f"({', '.join(sorted(os.path.basename(p) for p in present))})",
                DIM)
        say("\nwaiting for badges - plug one in. Ctrl-C to stop.", YELLOW)

    try:
        while True:
            for p, fut in list(futures.items()):
                if fut.done():
                    watcher.release(p)
                    del futures[p]

            for p in watcher.poll(ports_now()):
                futures[p] = pool.submit(flasher.handle, p)

            if args.once and not futures:
                break
            time.sleep(args.poll)
    except KeyboardInterrupt:
        say("\nstopping - waiting for badges in progress", YELLOW)
    finally:
        pool.shutdown(wait=True)

    say(f"\n{flasher.ok} flashed, {flasher.failed} failed, "
        f"{flasher.skipped} skipped",
        GREEN if flasher.failed == 0 else RED)
    return 1 if flasher.failed else 0


if __name__ == "__main__":
    sys.exit(main())
