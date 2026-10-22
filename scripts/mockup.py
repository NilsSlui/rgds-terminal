#!/usr/bin/env python3
"""Places a top and bottom screenshot onto docs/device.png's two screens.

    python3 mockup.py DEVICE.png TOP.png BOTTOM.png OUT.png [SCALE]

The screens are found automatically as the two large near-black rectangles in
the device image (one in each half). The result is SCALE times the size of the
device image (default 2) so the terminal text stays readable, and the white
background around the device is made transparent. Needs Pillow.
"""
import sys

from PIL import Image, ImageDraw, ImageFilter


def is_screen(p, dark=36, tint=14):
    """A pixel of the switched-off screen: near black and neutral, unlike the
    dark but coloured device body."""
    return max(p) < dark and max(p) - min(p) < tint


def longest_run(flags):
    """(start, end) of the longest run of True values."""
    best, start = (0, 0), None
    for i, f in enumerate(list(flags) + [False]):
        if f and start is None:
            start = i
        elif not f and start is not None:
            if i - start > best[1] - best[0]:
                best = (start, i)
            start = None
    return best


def find_screen(img, y0, y1, min_width=0.2):
    """Bounding box of the screen rectangle between rows y0 and y1."""
    px = img.load()
    w = img.width
    runs = {y: longest_run(is_screen(px[x, y]) for x in range(w)) for y in range(y0, y1)}
    rows = [y for y, (a, b) in runs.items() if b - a >= w * min_width]
    if not rows:
        raise SystemExit(f'no screen found between y={y0} and y={y1}')
    top, bottom = longest_run(y in rows for y in range(y0, y1))
    top, bottom = top + y0, bottom + y0
    x0, x1 = runs[(top + bottom) // 2]
    return x0, top, x1, bottom


def transparent_background(img, white=235, edge=3):
    """Makes the white background around the device transparent.

    Only white connected to the image border counts as background, so white
    details on the device itself stay opaque. Pixels along the outline are
    blended with white, so they get partial alpha with the white taken back out
    (like GIMP's colour-to-alpha), which avoids a light fringe on dark pages.
    """
    img = img.convert('RGB')
    w, h = img.size
    near_white = img.convert('L').point(lambda v: 255 if v >= white else 0)
    near_white.paste(255, (0, 0, w, 1))  # border rows/columns seed the fill
    near_white.paste(255, (0, h - 1, w, h))
    near_white.paste(255, (0, 0, 1, h))
    near_white.paste(255, (w - 1, 0, w, h))
    ImageDraw.floodfill(near_white, (0, 0), 128, thresh=0)
    background = near_white.point(lambda v: 255 if v == 128 else 0)
    outline = background.filter(ImageFilter.MaxFilter(2 * edge + 1))

    out = img.convert('RGBA')
    px, bg, ol = out.load(), background.load(), outline.load()
    for y in range(h):
        for x in range(w):
            if bg[x, y]:
                px[x, y] = (255, 255, 255, 0)
            elif ol[x, y]:
                r, g, b, _ = px[x, y]
                a = max(255 - r, 255 - g, 255 - b)
                if a == 0:
                    px[x, y] = (255, 255, 255, 0)
                elif a < 255:
                    k = 255 / a
                    px[x, y] = tuple(min(255, max(0, round(255 - (255 - c) * k)))
                                     for c in (r, g, b)) + (a,)
    return out


def main():
    device_path, top_path, bottom_path, out_path = sys.argv[1:5]
    scale = int(sys.argv[5]) if len(sys.argv) > 5 else 2
    device = Image.open(device_path).convert('RGB')
    h = device.height
    boxes = [find_screen(device, 0, h // 2), find_screen(device, h // 2, h)]

    out = device.resize((device.width * scale, device.height * scale), Image.LANCZOS)
    for shot_path, (x0, y0, x1, y1) in zip((top_path, bottom_path), boxes):
        print(f'{shot_path}: screen at {x0},{y0} - {x1},{y1} ({x1 - x0}x{y1 - y0})')
        box = [v * scale for v in (x0, y0, x1, y1)]
        shot = Image.open(shot_path).convert('RGB')
        out.paste(shot.resize((box[2] - box[0], box[3] - box[1]), Image.LANCZOS), box[:2])

    # Trim the white background around the device, keeping a small margin.
    mask = out.convert('L').point(lambda v: 255 if v < 245 else 0)
    x0, y0, x1, y1 = mask.getbbox()
    m = 12 * scale
    out = out.crop((max(0, x0 - m), max(0, y0 - m), min(out.width, x1 + m), min(out.height, y1 + m)))
    transparent_background(out).save(out_path, optimize=True)


if __name__ == '__main__':
    main()
