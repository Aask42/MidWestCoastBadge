# MidWestCoast Badge

A badge for an IoT Hacking BONANZA!

DEF CON 34 badge firmware for an ESP32-C3 driving a 240×320 ST7789 IPS panel
with a capacitive touch controller on I2C `0x15`. Everything is driven by swipes and
taps — there are no buttons.

---

## Build and flash

```sh
arduino-cli compile -u -p /dev/cu.usbmodemXXXX \
  --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc \
  --build-property upload.maximum_size=4063232 \
  badge
```

Find the port with `arduino-cli board list`. It changes between replugs.

**Both extra flags matter:**

| Flag | Why |
|---|---|
| `CDCOnBoot=cdc` | Puts the serial log on the USB port. Without it the sketch still builds, but the log moves to the UART pins. It also gates `Serial.setTxTimeoutMs(0)`, which is `#if`-guarded because that method only exists on `HWCDC`, not `HardwareSerial`. |
| `upload.maximum_size=4063232` | A **separate board property from the partition table**. Left alone it still reads 1.25 MB and will fail the build once assets grow, even though the real partition has room. Keep it equal to `app0`'s size in `partitions.csv`. |

### Dependencies

- `esp32:esp32` core **3.3.11** (the LEDC API here is 3.x — `ledcAttach(pin, freq, res)`)
- `GFX Library for Arduino` **1.6.7**

---

## Flash layout

`partitions.csv` in this directory **overrides the board's `PartitionScheme`
option entirely** — a sketch-local partition table wins over the FQBN menu
setting, so there is no scheme to pass on the command line.

| Partition | Size | Notes |
|---|---|---|
| `nvs` | 20 K | Settings. Same offset/size as stock, so **repartitioning does not wipe it**. |
| `otadata` | 8 K | Harmless; keeps the core's expectations satisfied. |
| `app0` | **3968 K** | The firmware. Stock gives 1280 K. |
| `coredump` | 64 K | How a crash on someone's lanyard gets diagnosed rather than guessed at. |

The chip is 4 MB (verified with `esptool flash_id`), so 3968 K is essentially
the ceiling. Two things the stock schemes spend it on that this badge does not
need:

- **`app1` (1.25 MB)** — a complete second copy of the firmware so an OTA
  update can roll back if it fails to boot. This badge flashes over USB. This
  is where most of the space comes from.
- **`spiffs` (896 K even on `huge_app`)** — a filesystem nothing here mounts.
  Settings live in NVS; image assets are better as `const` arrays in the app
  partition, where the flash cache maps them directly and they can be streamed
  to the panel without a read-into-RAM step.

The trade is **no over-the-air updates.** Current usage is ~1.05 MB (25 %),
leaving ~2.9 MB for lenticular image assets.

To read the live table back off a badge:

```sh
esptool --port /dev/cu.usbmodemXXXX read-flash 0x8000 0xc00 ptable.bin
python3 ~/Library/Arduino15/packages/esp32/hardware/esp32/3.3.11/tools/gen_esp32part.py ptable.bin
```

---

## Module map

`badge.ino` is entry point and wiring only. If logic accumulates there it
belongs in a module instead.

| Module | Owns |
|---|---|
| `config.h` | pins, geometry, palette, tuning constants. No code, no state — include it anywhere. |
| `types.h` | shared enums and the `Menu` struct. |
| `display` | panel, strip compositor, drawing primitives, screen transitions. |
| `input` | touch controller and gesture recognition. |
| `store` | everything persisted to NVS, and the two write policies. |
| `identity` | badge keypair and pairing secret. Separate NVS namespace. |
| `net` | WiFi association. Asynchronous throughout. |
| `keyboard` | the on-screen keyboard, shared by every text field. |
| `menus` | menu model, rendering, nav stack, row activation. |
| `modes` | the animated stereo scenes and mode dispatch. |
| `ui` | home screen, screen state, and what a gesture *means*. |

The one rule worth preserving: **`ui` is the only module that decides what a
gesture means.** Everything below it either reports raw input or draws what it
is told. That is what keeps the interaction rules readable in one place.

---

## Known issues

### Touch: intermittent, cause not yet pinned down

Be careful reading too much into any single capture here — an earlier version of
this section confidently blamed the register map, and the evidence since does
not support that. Recording what is actually known:

**The register map is probably correct.** Decoding the idle bytes
`00 00 00 40 DD 00 4C` with the CST816S layout gives x=221, y=76 — a valid
in-range coordinate held stale from a previous contact, with `FingerNum`
correctly reading 0. A wrong map would not produce sensible coordinates. One
capture also recorded **`down=81`**, so the decode demonstrably can register
contacts.

**ChipID `0xA3` reads `0x64` on every badge tested.** The CST816 family
documents `0xB4`/`0xB5`/`0xB6`, so this is either an undocumented variant or a
clone. Given the coordinates decode correctly, treat the ID as a curiosity
rather than as proof of anything.

**What is genuinely unresolved:** most captures show `down=0` for 20s at a time
even while the screen is being touched. Whether that is panel damage, an
intermittent flex, or a controller sleep state has not been established. The
`sleep(FE)` readback is always `0x00` despite writing `0x01` to it, so the
DisAutoSleep write is not sticking — that is the most promising thread to pull.

**Fixed and verified along the way:**

- **Touch polling was being starved by rendering.** With every mode animated, a
  full-screen redraw is ~40ms of solid SPI and the loop only reached
  `pollGesture()` about 25 times a second - a brisk tap could land entirely
  inside one frame. `present()` now polls between strips and stashes any
  completed gesture in `pendingGesture` for `loop()` to drain. Measured
  recovery: **~25 polls/sec to ~500 polls/sec** during animation.
- **The reset pulse was too short.** At 20ms/60ms the chip ACKed its address
  but every register read `0x00` - powered, address-decoding, core not running.
  50ms low plus 200ms settle made the ID registers readable.

**Tools already in `input.cpp`** for whoever picks this up: `busScan()`
(separates a stuck SDA from an empty bus from a real device), the three ID
registers in `touchDiag()` every 5s, and a raw 7-byte dump in `touchRead()`
that fires whenever any header byte goes non-zero.

### Other

- **MQTT is not implemented.** The menu stores config and shows the pairing
  secret; nothing connects. See `../PAIRING.md`.
- **The MQTT menu is full** at 7 rows (row 6 ends at y=280, nav bar at 284).
  A broker password field has nowhere to go without a paged menu.
- **Slideshow sync needs a clock.** Badges agree on phase via SNTP; with no
  network they free-run from `millis()` and drift.

---

## Lenticular images

The lens sends **even columns to one eye, odd columns to the other**. A
lenticular image is therefore two pictures sliced into alternating one-pixel
columns. `../tools/interlace.py` does that weave offline, so the badge carries
one interlaced result instead of both sources.

```sh
python3 ../tools/interlace.py --demo rings --name img_rings -o img_rings.h
python3 ../tools/interlace.py --left L.png --right R.png --name skull -o img_skull.h
python3 ../tools/interlace.py --image art.png --depth depth.png --name art -o img_art.h
```

Register it in `modes.cpp` (`IMAGE_DATA[]`, `IMAGE_NAMES[]`, `IMAGE_COUNT`) and
it appears in the SHOW menu and the slideshow rotation automatically.

**150KB each.** Two demo images moved the build from 1.20MB to 1.50MB of 3.97MB.

> Never scale a bitmap or offset it by an odd number of pixels — that swaps the
> eyes and inverts the depth. This is why images bypass the strip compositor and
> are blit whole.

---

## Menus paginate

`MENU_VISIBLE` is derived from the panel height (7 rows at current geometry).
Longer menus page rather than scroll:

- **the swipe that opened the menu** pages forward, wrapping — same direction
  you dragged it in from
- **the retreat swipe** still leaves, from any page
- the nav bar shows `p1/2` instead of the row counter when paging is active

Paging moves the *page*, never the highlight, so "swipes navigate, taps choose"
still holds. It is a no-op on menus that fit.

---

## MQTT

Transport is chosen by **port**, not by a separate setting — so moving from the
bench to HiveMQ is two fields on the MQTT screen, no reflash:

| Port | Transport |
|---|---|
| 1883 | plain TCP — local mosquitto |
| 8883 | TLS — HiveMQ Cloud |

### Topics

```
<topic>/badge/<id>/state    retained; who this badge is and what it shows
<topic>/badge/<id>/cmd      subscribed; commands in
```

`<topic>` is the MQTT screen's *topic* field (default `dc34`) and acts as a
namespace so two groups on one broker don't collide. `<id>` is the badge's
public-key fingerprint.

The retained `state` doc means a client connecting later still learns the badge
exists without waiting for a change. A **last-will clears it**, so a badge that
drops off doesn't leave a stale "I am here" behind.

### Verified working

Against a local mosquitto, full round trip:

```
mqtt: connected, subscribed dc34/badge/b00001a2/cmd
mqtt: published state to dc34/badge/b00001a2/state
mqtt: rx dc34/badge/b00001a2/cmd -> {"setName":"aask42"}
mqtt: name set to 'aask42'
```

Bench broker:

```sh
printf 'listener 1883 0.0.0.0\nallow_anonymous true\n' > /tmp/mosq.conf
mosquitto -c /tmp/mosq.conf -d
mosquitto_sub -h 127.0.0.1 -t 'dc34/#' -v
mosquitto_pub -h 127.0.0.1 -t 'dc34/badge/<id>/cmd' -m '{"setName":"aask42"}'
```

### HiveMQ Cloud

Set broker to `<cluster>.s1.eu.hivemq.cloud`, **port 8883**, plus the username
and password from the HiveMQ access-management page. TLS engages automatically.

> ⚠️ **`tlsClient.setInsecure()` is still in `mqtt.cpp`.** The connection is
> encrypted but the certificate is **not verified**, so it is not protected
> against an active man-in-the-middle. Fine on a bench; pin HiveMQ's CA before
> this is used for anything real.

Payloads are plaintext JSON today. The sealed-envelope scheme in
`../PAIRING.md` replaces that — which is why the pairing secret is already on
the MQTT screen.

---

## OTA updates

Publish a trigger and the badge pulls the new firmware itself:

```sh
arduino-cli compile --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc \
  --build-property upload.maximum_size=2031616 --output-dir out badge

python3 ../tools/ota_push.py --bin out/badge.ino.bin --broker 192.168.0.241
python3 ../tools/ota_push.py --bin out/badge.ino.bin --broker 192.168.0.241 \
        --badge 68cd2517          # one badge only
```

`ota_push.py` serves the image over HTTP, publishes the trigger, and waits for
each badge to come back reporting a new version. Or by hand:

```sh
mosquitto_pub -h 192.168.0.241 -t 'dc34/badge/<id>/cmd' \
  -m '{"ota":"http://192.168.0.241:8099/badge.ino.bin","md5":"<hex>"}'
```

**Verified working:** a badge on v0.4.0 was updated to v0.5.0 over the air with
no USB connection.

### This cost half the app partition

OTA fundamentally needs somewhere to write the new image before booting it, so
the partition table now carries **two** app slots instead of one:

| | app space | OTA |
|---|---|---|
| before | 3.875 MB (one slot) | no |
| now | 1.94 MB x2 | yes |

Firmware with two 150KB images is ~1.51MB, so **~78% of a slot is used** and
roughly three more images will fit. That is now the binding constraint.

If assets outgrow it, the fix is **not** a bigger app slot — it is moving
images into their own data partition so OTA only ever ships code. That needs
images loaded from a filesystem rather than PROGMEM.

`nvs` is unchanged at 0x9000/0x5000, so repartitioning preserved settings and
the badge keypair. **The first install of this table must be over USB** — a
badge on the old single-slot layout has nowhere to receive an OTA.

### Safety

- the running image is never overwritten; a failed download changes nothing
- size is checked against the free slot **before** any write
- an optional **MD5 is verified before the boot flag moves**, so a truncated
  download cannot be booted into
- failures are published (`{"ota":"failed","err":"..."}`) — a badge that
  silently refuses to update is worse than one that says why
- the badge shows a progress bar and `do not remove power`

> ⚠️ **Anyone who can publish to the broker can install firmware.** That is a
> far bigger deal than renaming a badge, and it is unauthenticated today. The
> sealed-envelope scheme in `../PAIRING.md` must land before this is used on a
> network you do not control. The image is fetched over plain HTTP too, so a
> MITM can swap it — the MD5 only protects against corruption, not an attacker,
> since they could change both.

---

## Battery telemetry

Every 30s each badge publishes to `<topic>/badge/<id>/telemetry`, **not
retained** — these are samples over time, and a retained one would sit on the
broker as a stale reading forever.

```json
{"id":"68cd2517","up":240,"mv":4425,"pct":94,"rssi":-75,
 "heap":151212,"show":"slideshow (all)","bright":100}
```

```sh
pip3 install paho-mqtt
python3 ../tools/battery_log.py --host 192.168.0.241 --csv run.csv
# HiveMQ:
python3 ../tools/battery_log.py --host x.hivemq.cloud --port 8883 --tls \
        --user badge --password secret
```

Appends every sample to CSV, prints a live table, and on Ctrl-C reports how
long each pack lasted.

**How runtime is measured.** A badge dying on battery cannot send a farewell —
the regulator drops out mid-frame — so runtime is inferred from *when it went
quiet*. The last sample before silence is the answer. `--dead-after` (default
120s) must stay comfortably above the firmware's 30s publish interval so a
single dropped packet is not mistaken for death. The logger also flags a badge
whose uptime went **backwards**, which is what a brownout reset looks like and
is easy to miss in a long log.

### The battery reading is unverified

`BATT_ADC_PIN` is **GPIO1**, which is an inference, not a schematic reading:
ADC1 is GPIO0-4, and 0/2/3/4 are already the backlight and touch, so GPIO1 is
the only candidate left. `BATT_DIVIDER` assumes 1:1.

Early evidence is encouraging — a badge on USB reported 4447 → 4442 → 4425 mV
over four minutes, which is stable and monotonic rather than the noise a
floating pin gives. But **on USB that could equally be the supply rail rather
than the pack**. Running on AAAs and watching it fall settles it.

`batteryValid()` rejects anything outside 1.5-6.0V, so a badge with no sense
circuit shows `--` and reports `pct: -1` rather than inventing a percentage.
The SYSTEM menu distinguishes `battery: on (94%)` from `battery: on (no
sense)` for the same reason.

Percentages assume 3xAAA alkaline, 4.5V full to 3.2V flat, interpolated
linearly. Alkaline discharge is **not** linear, so treat `pct` as a coarse
gauge and `mv` as the real data.

The icon draws bottom-right over home and modes only — a menu has its nav bar
there. Toggle it from **SYSTEM → battery**.

---

## Serial console

Provisioning a badge by typing a WPA2 passphrase on a 240px on-screen keyboard
is miserable, and it is the first thing anyone does with a new badge. Over USB
instead:

```
wifi <ssid> <pass>              set credentials and join
mqtt <host> <port> [user] [pw]  set broker and reconnect
name <text>                     set the nametag
show                            print current config
help
```

```sh
stty -f /dev/cu.usbmodemXXXX 115200 raw -echo
printf 'wifi MyNet hunter2\n'          > /dev/cu.usbmodemXXXX
printf 'mqtt 192.168.0.241 1883\n'     > /dev/cu.usbmodemXXXX
printf 'show\n'                        > /dev/cu.usbmodemXXXX
```

Compiled out entirely when `DEBUG_SERIAL` is 0, so a production build has no
serial control surface at all.

---

## Wiring

| Display (conn. 7) | Pin | | Touch (conn. 8) | Pin |
|---|---|---|---|---|
| SCK | 6 | | SDA | 4 |
| MOSI | 7 | | SCL | 5 |
| DC | 10 | | INT | 3 |
| RST | 8 | | RST | 2 |
| BL (LEDK) | 0 | | I²C addr | `0x15` |

Display **CS is strapped to GND on the PCB**, so it is passed as
`GFX_NOT_DEFINED`. The backlight is driven by LEDC PWM (5 kHz, 8-bit) for
brightness control, falling back to a plain digital high if `ledcAttach` fails —
a bug in the dimmer should never leave you with a black screen.

---

## Navigation

### Screen flow

```
boot ──► DEF CON 34 splash ──► home ──► (idle 15 s) ──► selected mode
                                 ▲                          │
                                 └────── tap ───────────────┘
```

A **tap** while a mode is running dismisses it to home. A **swipe** dismisses it
*and still opens the menu it was aimed at* — waking the badge never costs you a
gesture.

### Menus are anchored to screen edges

Which menu opens is decided by the **edge the stroke started at**, not the
direction it travelled. At the bottom edge a finger can only move up, so
direction alone would open the top menu — exactly the mismatch this avoids.
Strokes starting mid-screen (>80 px from any edge) fall back to direction.

| Edge | Menu | Leave with |
|---|---|---|
| top | **MODE** | swipe up |
| bottom | **IoT CONFIG** | swipe down |
| left | **SETTINGS** | swipe left |
| right | **SYSTEM** | swipe right |

### Inside a menu: swipes move between screens, taps choose items

Every row of every menu is on screen at once, so **nothing scrolls** and a swipe
never moves the highlight. The nav bar at the bottom of each menu names the one
swipe that leaves it, and that swipe leaves **from any row**.

- **Tap a row** → acts on it immediately (opens the keyboard, toggles a value…)
- **Swipe the retreat direction** → back to whatever screen opened this one
- **Any other swipe** → ignored, and logged with what would have worked

**The MODE menu is the one exception.** Its rows are a choice rather than an
action, so the first tap only moves the highlight (a mis-tap costs nothing) and
tapping the highlighted row *again* confirms it and returns home.

---

## Menus

### MODE

`slideshow` · `nametag` · `static image` · `lenticular cube`

The selection *is* the setting — `activeMode()` reads it directly.

### SETTINGS

`set name` (keyboard) · `clear name`

### IoT CONFIG

`broker` · `port` · `user` · `topic` · `status` (read-only) · `id`

Rows show live values. `port` opens a numeric-only keyboard and is clamped to
1–65535 on commit. `id` regenerates a random per-badge MQTT client ID so two
badges don't fight over one session. **The MQTT client itself is not wired up
yet** — `status` always reads offline.

### SYSTEM

`brightness` · `wifi` · `pw` · `status` (read-only) · `connect`/`disconnect`

- **brightness** — tap cycles 15/30/50/75/100 %, applied instantly via PWM.
- **wifi / pw** — open the keyboard. The passphrase is **masked in the list**
  (a badge on a table shouldn't display its own password) but shown in clear
  while editing, where you need to check what you typed.
- **connect** — `WiFi.begin` is asynchronous throughout, so a wrong password
  costs you a status label, not a frozen UI. Auto-joins at boot if an SSID is
  already stored. Status is polled once a second and only redraws when SYSTEM
  is the visible screen.

---

## On-screen keyboard

One keyboard serves every text field. Opening it on a new field is a single
`kbOpen(target, size, title, numeric, onCommit)` call.

```
        ┌──────────────────────────────┐
   20   │  WIFI PASSWORD               │  title
   34   │ ┌──────────────────────────┐ │
        │ │ hunter2▌                 │ │  entry field + caret
   74   │ └──────────────────────────┘ │
   78   │  ← del  ↓ back      41 left  │  hint / counter
   92   │ [   ◄   ][   ►   ][   CLR  ] │  caret row
  128   │ [1][2][3][4][5][6][7][8][9][0]│
        │ [q][w][e][r][t][y][u][i][o][p]│  key grid
        │  [a][s][d][f][g][h][j][k][l]  │
        │ [z][x][c][v][b][n][m][.][-][_]│
  272   │ [DEL][abc][!#/][SPACE ][ OK ] │  action row
  320   └──────────────────────────────┘
```

- **Text inserts at the caret**, so a typo mid-string is a fix, not a retype.
- **`!#/`** flips to a second page with all 32 ASCII punctuation marks in ASCII
  order. Combined with the letter page, every printable ASCII character is
  reachable — which is what makes MQTT topics like `dc34/badge/+` typable.
- **`abc`** cycles `abc → Abc → ABC`. `Abc` is one-shot and applies to exactly
  the next character; `ABC` sticks. It's a three-state cycle rather than
  double-tap-for-caps because double-tap timing is invisible to the user.
- **Swipe left** deletes a character — the same direction the caret moves.
- **Swipe down** cancels, pushing the keyboard back to the edge it rose from.
- Entry length is capped by the **destination buffer**, so what you can type is
  exactly what will be stored. The counter shows what's left.

The action row is deliberately **flush with the bottom edge of the panel**. It
used to stop 36 px short, and every missed tap in the touch logs landed in that
strip, all of them aimed at DEL or OK — a finger reaching for a bottom corner
goes to the actual bottom.

---

## Persistence

Settings live in NVS under the `badge` namespace via `Preferences`.

- **Menu selections and incidental changes** are debounced 1.5 s, so scrolling a
  list doesn't burn a flash write per row.
- **Deliberate one-shot commits** — a name, a chosen mode, a brightness step —
  are flushed **immediately**. Losing one to a battery pull in a 1.5 s window is
  not an acceptable trade for a badge.

NVS is log-structured, so overwriting a key appends a new entry and the old one
lingers until compaction. Dumping the partition will show several stale
`cid` values; that's normal, and the newest one is what reads back.

---

## Rendering

A full-screen canvas would be 240 × 320 × 2 = 153,600 bytes, which will not
allocate as one contiguous block on this C3. Instead the screen is composited
**one 80-px horizontal strip at a time** and each strip is blitted straight to
the panel. It's still flicker-free, because every pixel is written exactly once
per frame.

Screen changes animate as a real slide: the outgoing and incoming screens are
composited at an offset into the strip buffer. If the strip buffer won't
allocate, the badge still runs — it just cuts between screens instead of sliding.

**Partial repaints matter.** A keystroke changes a few hundred pixels; routing
it through a full `render()` rebuilt all 240×320 (~150 KB of SPI, ~36 ms) and
made typing feel sluggish. Each key now reports how much it actually
invalidates:

| Key | Repaint |
|---|---|
| letters, digits, space, DEL, caret moves, CLR | entry band only |
| case key, symbols page | full — every key face changes |
| one-shot shift falling back to lowercase | full — this re-letters the whole grid |

That last row is the subtle one: it would be easy to repaint just the field and
leave the keys showing the wrong case.

---

## Lenticular cube

The lens over the panel sends **even columns to one eye and odd columns to the
other**. A rotating wireframe cube is rendered as a genuine stereo pair — two
independent projections interleaved by column parity — so it has real binocular
depth rather than a painted-on illusion.

- `projectCube()` — yaw about Y, pitch about X at 0.6× the rate so the tumble
  doesn't repeat every revolution. Convergence is set at `CUBE_DIST`, so
  geometry at that depth lands on the glass with zero parallax and everything
  else splits either side of it.
- `drawLineParity()` — Bresenham that plots only even or only odd columns.

**`EYE_SEP` is the knob to tune against your physical lens.** Disparity is
`EYE_SEP × CUBE_FOCAL × (1/z − 1/CUBE_DIST)`; at the current `0.28` that spans
about −3 px (behind the glass) to +6 px (in front). Much past that and the two
images stop fusing and read as ghosting instead of depth.

Left eye is cyan, right is amber, which makes this double as an **alignment
check**: through a correctly-pitched lens each eye sees one solid colour, and a
misaligned lens shows magenta blend or shimmer. Set `C_EYE_L` and `C_EYE_R` to
the same value for a straight render with no colour rivalry.

> ⚠️ `drawLenticular()` is called **once per strip**, four times per frame. The
> rotation angle is computed once per frame in `loop()` and held in a global —
> deriving it from `millis()` inside the draw call would advance it between
> strips and shear the cube into four misaligned bands.

---

## Touch handling

Two things here are load-bearing and were both found from real touch logs:

**One threshold, not two.** A completed stroke is a swipe if it travelled
≥ 22 px on its dominant axis and a **tap otherwise**. These used to be separate
constants (`SWIPE_MIN = 22`, `TAP_MAX = 12`), leaving a 10 px band where a
stroke was classified as neither and silently discarded — and a fingertip
drifts right into that band on a perfectly ordinary tap. **No stroke is ever
thrown away now.**

**Glitch rejection.** The controller occasionally emits a wild coordinate.
Because travel is tracked as the furthest point ever seen, one bad sample turned
a short stroke into a full swipe — the logs caught a 46 px stroke reporting
173 px of travel. Samples are milliseconds apart, so any jump over 60 px is
discarded as a glitch; the stroke itself survives.

Also note `T_FAILED` is distinguished from `T_UP`: treating a failed I²C read as
"finger lifted" chops a swipe in half and loses the gesture.

---

## Debugging

Set `DEBUG_SERIAL 0` in `badge.ino` to compile out all per-gesture logging.

`arduino-cli monitor` does not stay attached when stdout isn't a terminal.
To capture a log to a file, talk to the tty directly:

```sh
stty -f /dev/cu.usbmodemXXXX 115200 raw -echo
cat /dev/cu.usbmodemXXXX > touch.log
```

Every stroke logs as:

```
stroke start=98,88 end=98,88 best=0,0 -> TAP
select+activate: SETTINGS / set name
edit 'NAMETAG' start, value 'YOUR NAME'
```

`start`/`end` are raw panel coordinates and `best` is the furthest travel seen,
which is what the gesture classifier actually keys off. If a tap on the top row
reports `y≈250` instead of `y≈70`, the touch Y axis is inverted relative to the
display.

> The capture holds the port open — release it before flashing.

---

## Tuning constants

| Constant | Value | What it does |
|---|---|---|
| `SWIPE_MIN` | 22 px | Swipe/tap threshold |
| `MAX_JUMP` | 60 px | Glitch rejection ceiling |
| `EDGE_ZONE` | 80 px | How close to an edge a stroke counts as that edge's |
| `SPLASH_MS` | 3000 | Boot splash |
| `IDLE_MS` | 15000 | Home idle before handing over to the mode |
| `SLIDE_MS` | 2500 | Slideshow dwell per image |
| `SAVE_DEBOUNCE_MS` | 1500 | NVS write debounce |
| `ANIM_STEPS` | 3 | Frames per screen transition |
| `LENT_FRAME_MS` | 40 | Cube frame interval (~20 fps, bounded by redraw) |
| `LENT_PERIOD_MS` | 6000 | One full cube rotation |
| `EYE_SEP` | 0.28 | Stereo strength — **tune against your lens** |
| `STRIP_H` | 80 px | Composite strip height |

---

## Not wired up yet

- **MQTT.** The IoT menu stores broker/port/user/topic and generates a client
  ID, but nothing connects. `status` always reads offline.
- **Slideshow assets.** Currently procedural test patterns. There is ~2.9 MB of
  app partition waiting for real images.
- **MQTT password.** No field for it yet.
