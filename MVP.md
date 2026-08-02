# Minimum Viable Product

Show images meant to be seen through a lenticular lens being 3d printed in clear

Basic touch screen actions. Swipe down to see mqtt creds, up to set mode (slideshow, nametag, static image w.select)

Image Slideshow
Nametag
Auto-cut bitmaps into interlaced images. Have requirement for images embedded in device as we are limited in size
MQTT Nametag Setting, companion web-serial app in Chrome.

Device: esp32-c3-h4


Pinout: ![alt text](image.png)

Use micropythyon:

/Users/ameliawietting/dev/2026_iwp_temporal <- for wifi management, mqtt management, and how to do background timers

https://github.com/lvgl-micropython/lvgl_micropython <- for the screen driver for mpy
https://gist.github.com/rena2019/bd8c6d5ca7b3fbb5dd576376494b9d65 <- For how to do other things with the screen in mpy

---

# Status

The Arduino/C++ firmware in `arduino/badge/` is the live implementation.
MicroPython above is the earlier exploration and is not being carried forward.

### Original MVP checklist

| Item | State |
|---|---|
| Show images through a lenticular lens | ✅ 5 animated stereo scenes + embedded bitmaps |
| Basic touch actions, swipe to menus | ✅ edge-anchored menus, taps choose |
| Image slideshow | ✅ cycles every scene and bitmap |
| Nametag | ✅ |
| **Auto-cut bitmaps into interlaced images** | ✅ `tools/interlace.py` |
| **Images embedded in device** | ✅ PROGMEM, ~150KB each |
| MQTT nametag setting | ✅ verified against mosquitto |
| **Companion app** | ⬜ designed in [PAIRING.md](PAIRING.md), not built |

Also done since: on-screen keyboard, WiFi, brightness, NVS persistence, badge
keypair + pairing secret, SNTP slideshow sync, serial provisioning console,
paged menus, factory reset with confirm + reboot.

**Remaining:** the companion web app. The original sketch said web-serial in
Chrome; now that MQTT works, [PAIRING.md](PAIRING.md) specifies an MQTT-over-
WebSockets client instead, which works from any browser and does not need the
badge plugged in.

### Image pipeline

`tools/interlace.py` weaves a stereo pair into one frame — even columns to the
left eye, odd to the right — and emits a C header of RGB565 ready to blit.

```sh
# real stereo pair
python3 tools/interlace.py --left L.png --right R.png --name skull -o arduino/badge/img_skull.h

# flat art plus a greyscale depth map
python3 tools/interlace.py --image art.png --depth art_depth.png --name art -o arduino/badge/img_art.h

# no art needed
python3 tools/interlace.py --demo rings --name img_rings -o arduino/badge/img_rings.h
```

Then add it to `IMAGE_DATA[]` / `IMAGE_NAMES[]` in `modes.cpp` and bump
`IMAGE_COUNT`.

**Budget:** 240x320 RGB565 is 150KB per image. Two demo images took the build
from 1.20MB to 1.50MB of the 3.97MB partition, so there is room for roughly 16
more. Check the compiler's number rather than trusting that estimate.

---

# Requested — lenticular content

*Captured 2026-07-28.*

## More lenticular art, all of it animated

Everything shown through the lens should be **stereo and moving**. A flat image
under a lenticular lens looks like a slightly blurry flat image; the lens only
earns its place if the two eyes are being fed different views.

Scenes to build out (each rendered as a genuine stereo pair, column-interleaved
so even columns go to one eye and odd to the other):

- rotating cube ✅ *(built)*
- pyramid / tetrahedron
- depth tunnel or starfield — strongest pop-out effect for the least geometry
- concentric rings floating at different depths
- "DEF CON 34" text standing off the background plane

## Static mode = pick one scene, still animated

"Static" means **stop cycling**, not stop moving. A frozen frame is the one
thing that looks bad through the lens. So:

- **Slideshow mode** — cycle through every scene
- **Static mode** — hold one chosen scene, still animating

**Built.** Resolved by collapsing the split rather than adding a second setting:
the SHOW menu (top edge) is now one flat list — `nametag`, `slideshow (all)`,
then every animation by name. Picking an animation by name *is* static mode, so
there is no separate "which scene?" control to go looking for.

## Tap the home/splash screen to start showing — **built**

*Requested 2026-07-28.*

A tap on the home card (or the splash) now runs **whatever mode the card is
currently showing**, rather than slideshow specifically. The card already names
the selected mode in its centre, so the gesture reads as "show me that" instead
of as a hidden shortcut, and the card carries a `tap to show` hint.

Combined with tap-to-leave-a-mode, tap is a clean toggle: home → mode → home.

## All badges synced in slideshow mode

Two badges sitting side by side should change scene at the same instant.

> **On the two badges that already look synced:** that is a coincidence worth
> understanding rather than relying on. Animation phase is currently derived
> from `millis()`, which counts from boot. Both badges were reset within a few
> seconds of each other, so they free-run nearly in step — and because the ESP32
> crystal is good to roughly ±20ppm, the phase drifts apart only slowly. It
> looks like sync and behaves like sync for a while, but nothing is
> co-ordinating them, and they will separate.

Making it real needs a **shared clock, not a shared connection** — the badges
never talk to each other:

1. Derive animation phase from **absolute time**, not uptime. Any two badges
   agreeing on the wall clock then agree on the phase, with no traffic between
   them at all.
2. Get that clock from **SNTP** once WiFi is up. Sub-second accuracy is far
   more than a 2.5s scene dwell needs.
3. Fall back to `millis()` when there is no clock — i.e. today's behaviour,
   which is why it already looks right.

Later, an MQTT time broadcast could sync badges with no internet, using the same
phase function. The point of doing it this way is that badges with a clock stay
in lockstep **indefinitely**, and badges without one degrade to exactly what
happens now.
