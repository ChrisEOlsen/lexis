#!/usr/bin/env python3
"""Generates the LEXIS app icon: a macOS rounded-square (squircle-radius
approximation) in near-black with a pixel-art smiley in accent blue.
Writes app/assets/LEXIS.icns via an intermediate .iconset. Re-run after
editing the pixel grid below; the .icns is committed so builds don't
need Python."""
import subprocess, tempfile, os
from PIL import Image, ImageDraw

S = 1024
CELL = 64  # 16x16 pixel-art grid
BG_TOP = (18, 18, 22)
BG_BOTTOM = (8, 8, 10)
BLUE = (10, 132, 255)  # macOS accent blue
RADIUS = int(S * 0.2237)  # Apple's rounded-square corner ratio

img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
draw = ImageDraw.Draw(img)

# Vertical gradient background inside a rounded rect
grad = Image.new("RGBA", (S, S))
gd = ImageDraw.Draw(grad)
for y in range(S):
    t = y / S
    c = tuple(int(BG_TOP[i] + (BG_BOTTOM[i] - BG_TOP[i]) * t) for i in range(3))
    gd.line([(0, y), (S, y)], fill=c + (255,))
mask = Image.new("L", (S, S), 0)
ImageDraw.Draw(mask).rounded_rectangle([0, 0, S - 1, S - 1], radius=RADIUS, fill=255)
img.paste(grad, (0, 0), mask)

# Pixel-art smiley: eyes 2x2, three-tier smile, on the 16x16 grid
cells = [
    # left eye        right eye
    (5, 5), (6, 5),   (9, 5), (10, 5),
    (5, 6), (6, 6),   (9, 6), (10, 6),
    # smile
    (4, 9),                   (11, 9),
    (5, 10),                  (10, 10),
    (6, 11), (7, 11), (8, 11), (9, 11),
]
PAD = 6  # tiny inset so adjacent cells read as distinct "pixels"
for cx, cy in cells:
    x0, y0 = cx * CELL + PAD, cy * CELL + PAD
    draw.rectangle([x0, y0, x0 + CELL - 2 * PAD, y0 + CELL - 2 * PAD], fill=BLUE + (255,))

out_png = "app/assets/icon_1024.png"
img.save(out_png)

with tempfile.TemporaryDirectory() as td:
    iconset = os.path.join(td, "LEXIS.iconset")
    os.mkdir(iconset)
    for size in (16, 32, 128, 256, 512):
        for scale in (1, 2):
            px = size * scale
            name = f"icon_{size}x{size}" + ("@2x" if scale == 2 else "") + ".png"
            img.resize((px, px), Image.LANCZOS).save(os.path.join(iconset, name))
    subprocess.run(["iconutil", "-c", "icns", iconset, "-o", "app/assets/LEXIS.icns"], check=True)
print("wrote app/assets/LEXIS.icns and", out_png)
