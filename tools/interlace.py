#!/usr/bin/env python3
"""Cut bitmaps into a lenticular-interlaced image embedded in the firmware.

The lens over the badge sends EVEN screen columns to one eye and ODD columns to
the other. So a lenticular image is not one picture - it is two, sliced into
alternating one-pixel columns. This tool does that slicing offline, because
doing it on the badge would mean carrying both source images in flash instead
of one interlaced result.

Usage
-----
  # a real stereo pair (two photographs / renders of the same scene)
  interlace.py --left L.png --right R.png --name skull -o out/skull.h

  # one flat image, given synthetic depth from a greyscale depth map
  interlace.py --image art.png --depth art_depth.png --name art -o out/art.h

  # one flat image, no depth map: whole picture floats at a fixed offset
  interlace.py --image art.png --shift 6 --name art -o out/art.h

  # built-in demo scene, no input files needed
  interlace.py --demo rings --name rings -o out/rings.h

Output is a C header holding one `const uint16_t[]` in PROGMEM-friendly form,
already in the panel's RGB565 byte order, ready to blit with
`draw16bitRGBBitmap()`.

Size
----
240x320 RGB565 is 153,600 bytes per image. The app partition has ~2.9MB spare,
so budget roughly 18 images absolute maximum and far fewer in practice. Check
what the firmware reports after a build rather than guessing.
"""

import argparse
import os
import sys

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow is required:  pip3 install Pillow")

W, H = 240, 320


def rgb565(r, g, b):
    """Pack 8-8-8 into the panel's 5-6-5."""
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def fit(img):
    """Cover-fit to the panel: scale to fill, then centre-crop the overflow.

    Cover rather than contain, because letterbox bars under a lenticular lens
    look like a fault in the lens rather than a deliberate border.
    """
    img = img.convert("RGB")
    scale = max(W / img.width, H / img.height)
    new = (max(1, round(img.width * scale)), max(1, round(img.height * scale)))
    img = img.resize(new, Image.LANCZOS)
    left = (img.width - W) // 2
    top = (img.height - H) // 2
    return img.crop((left, top, left + W, top + H))


def shift_from_depth(img, depth, max_shift):
    """Build a stereo pair by displacing pixels horizontally by depth.

    A real depth-image-based renderer fills the gaps it opens up behind objects.
    This does not - it just smears the last written column, which is fine for
    the shallow parallax a 1px-pitch lens can resolve (a few px) and visibly
    wrong if you push max_shift high. Prefer a true stereo pair for hero art.
    """
    depth = depth.convert("L").resize((W, H), Image.LANCZOS)
    src = img.load()
    dpx = depth.load()

    left = Image.new("RGB", (W, H))
    right = Image.new("RGB", (W, H))
    lpx = left.load()
    rpx = right.load()

    for y in range(H):
        for x in range(W):
            # 0 = far (zero parallax), 255 = near (full pop-out).
            d = dpx[x, y] / 255.0
            off = int(round(d * max_shift * 0.5))
            c = src[x, y]
            lx = min(W - 1, max(0, x - off))
            rx = min(W - 1, max(0, x + off))
            lpx[lx, y] = c
            rpx[rx, y] = c
    return left, right


def demo_scene(kind):
    """Generate a stereo pair without needing any input art."""
    import math

    left = Image.new("RGB", (W, H), (8, 8, 16))
    right = Image.new("RGB", (W, H), (8, 8, 16))
    lpx, rpx = left.load(), right.load()

    if kind == "rings":
        # Concentric rings at descending depths: the innermost pops furthest.
        rings = [(70, 7, (0, 255, 255)), (52, 4, (255, 160, 0)),
                 (34, 1, (255, 255, 255)), (16, -3, (255, 0, 128))]
        cx, cy = W // 2, H // 2
        for rad, par, col in rings:
            for a in range(0, 3600):
                t = a * math.pi / 1800.0
                x = cx + rad * math.cos(t)
                y = cy + rad * math.sin(t)
                for th in range(-2, 3):
                    for eye, px in ((-1, lpx), (1, rpx)):
                        xx = int(round(x + th + eye * par * 0.5))
                        yy = int(round(y))
                        if 0 <= xx < W and 0 <= yy < H:
                            px[xx, yy] = col
    elif kind == "grid":
        # A floor grid receding to a horizon: strong, cheap depth.
        for i in range(1, 26):
            z = i * 0.6
            par = int(round(14 / z))
            y = int(H * 0.55 + 900 / (z * 6))
            if 0 <= y < H:
                for x in range(W):
                    for eye, px in ((-1, lpx), (1, rpx)):
                        xx = x + eye * par // 2
                        if 0 <= xx < W:
                            px[xx, y] = (0, 200, 255)
        for k in range(-12, 13):
            for step in range(0, 200):
                z = 0.6 + step * 0.08
                par = int(round(14 / z))
                x = int(W / 2 + k * 9 * (3.0 / z))
                y = int(H * 0.55 + 900 / (z * 6))
                if 0 <= x < W and 0 <= y < H:
                    for eye, px in ((-1, lpx), (1, rpx)):
                        xx = x + eye * par // 2
                        if 0 <= xx < W:
                            px[xx, y] = (255, 120, 0)
    else:
        sys.exit(f"unknown demo scene '{kind}' (try: rings, grid)")
    return left, right


def interlace(left, right):
    """Weave two views into one frame: even columns left eye, odd columns right.

    This column parity is the ONLY thing separating the two eyes, and it has to
    match the firmware's drawLineParity() convention exactly - even = left.
    """
    out = Image.new("RGB", (W, H))
    op = out.load()
    lp, rp = left.load(), right.load()
    for y in range(H):
        for x in range(W):
            op[x, y] = lp[x, y] if (x & 1) == 0 else rp[x, y]
    return out


def emit_raw(img, path):
    """Raw RGB565, little-endian, row-major - exactly what the panel wants.

    No header, no palette, no compression: the badge streams this straight from
    LittleFS to the display in bands, so any framing would just be something to
    skip past on every read.
    """
    px = img.load()
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "wb") as f:
        for y in range(H):
            row = bytearray()
            for x in range(W):
                v = rgb565(*px[x, y])
                row += bytes((v & 0xFF, v >> 8))   # little-endian
            f.write(row)
    return W * H * 2


def emit_header(img, name, path, source_note):
    px = img.load()
    words = [rgb565(*px[x, y]) for y in range(H) for x in range(W)]

    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w") as f:
        f.write(f"// {name}.h - generated by tools/interlace.py. Do not edit.\n")
        f.write(f"//\n// Source: {source_note}\n")
        f.write("//\n// Lenticular-interlaced: EVEN columns are the left-eye\n")
        f.write("// view, ODD columns the right-eye view. Blit whole with\n")
        f.write("// draw16bitRGBBitmap(); never scale or offset it by an odd\n")
        f.write("// number of pixels or the eyes swap and the depth inverts.\n")
        f.write("#pragma once\n#include <Arduino.h>\n\n")
        f.write(f"#define {name.upper()}_W {W}\n")
        f.write(f"#define {name.upper()}_H {H}\n\n")
        f.write(f"const uint16_t {name}_data[] PROGMEM = {{\n")
        for i in range(0, len(words), 12):
            row = ", ".join(f"0x{w:04X}" for w in words[i:i + 12])
            f.write(f"    {row},\n")
        f.write("};\n")
    return len(words) * 2


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--left")
    ap.add_argument("--right")
    ap.add_argument("--image")
    ap.add_argument("--depth")
    ap.add_argument("--shift", type=int, default=6,
                    help="max parallax in px when using --depth or a flat "
                         "--image. Past ~8px the eyes stop fusing and it reads "
                         "as ghosting rather than depth.")
    ap.add_argument("--demo", help="built-in scene: rings, grid")
    ap.add_argument("--name", required=True, help="C identifier for the array")
    ap.add_argument("-o", "--out", required=True,
                    help=".bin for the LittleFS image partition (preferred), "
                         "or .h to bake into the firmware (legacy)")
    ap.add_argument("--preview", help="also write the interlaced PNG here")
    a = ap.parse_args()

    if a.demo:
        left, right = demo_scene(a.demo)
        note = f"built-in demo scene '{a.demo}'"
    elif a.left and a.right:
        left, right = fit(Image.open(a.left)), fit(Image.open(a.right))
        note = f"stereo pair {os.path.basename(a.left)} / {os.path.basename(a.right)}"
    elif a.image and a.depth:
        base = fit(Image.open(a.image))
        left, right = shift_from_depth(base, Image.open(a.depth), a.shift)
        note = f"{os.path.basename(a.image)} + depth map, max shift {a.shift}px"
    elif a.image:
        base = fit(Image.open(a.image))
        left = base.transform((W, H), Image.AFFINE, (1, 0, a.shift / 2, 0, 1, 0))
        right = base.transform((W, H), Image.AFFINE, (1, 0, -a.shift / 2, 0, 1, 0))
        note = f"{os.path.basename(a.image)}, flat plane at {a.shift}px offset"
    else:
        ap.error("need --demo, or --left/--right, or --image [--depth]")

    woven = interlace(left, right)
    if a.preview:
        woven.save(a.preview)
    if a.out.endswith(".h"):
        size = emit_header(woven, a.name, a.out, note)
    else:
        size = emit_raw(woven, a.out)
    print(f"{a.out}: {a.name} {W}x{H} RGB565, {size} bytes ({size/1024:.1f} KB)")
    print(f"  source: {note}")


if __name__ == "__main__":
    main()
