"""Rasterize the original Yiyi vector mark into the Windows ICO sizes.

Requires Pillow only for development; the application uses no Python runtime.
"""
from pathlib import Path
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
SCALE = 4
image = Image.new("RGBA", (128 * SCALE, 128 * SCALE))
draw = ImageDraw.Draw(image)


def points(coords):
    return [(round(x * SCALE), round(y * SCALE)) for x, y in coords]


def path(start, commands, fill):
    output = [start]
    current = start
    for command in commands:
        if len(command) == 2:
            current = command
            output.append(current)
        else:
            x0, y0 = current
            x1, y1, x2, y2, x3, y3 = command
            for i in range(1, 25):
                t = i / 24
                u = 1 - t
                output.append((u**3*x0 + 3*u*u*t*x1 + 3*u*t*t*x2 + t**3*x3,
                               u**3*y0 + 3*u*u*t*y1 + 3*u*t*t*y2 + t**3*y3))
            current = (x3, y3)
    draw.polygon(points(output), fill=fill)


draw.rounded_rectangle((0, 0, 128*SCALE-1, 128*SCALE-1), 34*SCALE, fill="#347457")
path((28, 36), [(63, 36), (76, 36, 86, 46, 86, 59), (86, 88), (55, 88),
                (40, 88, 28, 76, 28, 61)], "#eaf3d9")
path((100, 36), [(72, 36), (58, 36, 47, 47, 47, 61), (47, 93), (61, 81),
                 (74, 81), (88, 81, 100, 69, 100, 55)], "#acd2b5")
path((47, 60), [(47, 47, 56, 37, 68, 36), (79, 38, 86, 47, 86, 59),
                (86, 81), (61, 81), (47, 93)], "#f5f7e9")
for line in [[(58, 53), (75, 53)], [(66.5, 47), (66.5, 53)],
             [(61, 54), (62, 60), (66, 66), (73, 71)],
             [(73, 54), (71, 61), (66, 67), (59, 72)]]:
    draw.line(points(line), fill="#347457", width=round(3.5*SCALE), joint="curve")
    for x, y in [line[0], line[-1]]:
        r = 1.75*SCALE
        draw.ellipse((x*SCALE-r, y*SCALE-r, x*SCALE+r, y*SCALE+r), fill="#347457")

image.resize((256, 256), Image.Resampling.LANCZOS).save(
    ROOT / "assets" / "app.ico", sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]
)
print("Generated assets/app.ico")
