"""Builds the repository banner and mark from the black hole render.

The banner is wider than the render is, so the hole cannot simply be scaled to
fit: doing that would either squash it out of round or leave the sides empty.
Instead the render is scaled to the banner's height and placed to the right,
and the space to its left is filled by mirroring the outer starfield, which
carries the lensed arcs outward without repeating anything recognisable.

Run from the repository root:

    python brand/make_banner.py <source image>
"""

from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parent.parent
BRAND = ROOT / "brand"

BANNER = (1280, 480)
MARK = 512

# Sampled from the render rather than taken from the theme.
#
# The application's accent is a teal, which is right on a dark window and
# wrong here: this disk is almost colourless, a cool silver, and a teal chip
# sitting on it reads as a mistake. The brightest part of the disk measures
# #b4bbcc and the body of it #8892a6, so the words use those, lifted just
# enough to stay legible against the black.
TEXT = (238, 244, 251)
MUTED = (136, 146, 166)
ACCENT = (205, 214, 230)


def load_font(size: int, bold: bool = False) -> ImageFont.FreeTypeFont:
    """Poppins if the machine has it, otherwise whatever Windows ships."""
    names = (
        ["poppinssemibold.ttf", "seguisb.ttf", "segoeuib.ttf", "arialbd.ttf"]
        if bold
        else ["poppins.ttf", "segoeui.ttf", "arial.ttf"]
    )
    for name in names:
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            continue
    return ImageFont.load_default()


# How wide the render is drawn before the banner is cut out of it.
#
# Larger than the banner on purpose. The hole sits in the middle of the
# render, so taking the banner from the render's left half is what puts the
# hole right of centre and leaves the words a clear space.
RENDER_WIDTH = 1150

# How far in the fade to black reaches. Far enough to clear the longest line
# of text and no further.
TEXT_ZONE = 520


def widen(source: Image.Image, size: tuple[int, int]) -> Image.Image:
    """Fits the render into a wider frame without distorting the hole.

    Nothing is mirrored or invented. The disk in this render already sweeps
    left and right from the hole, so the banner is a horizontal band cut out
    of it: the hole keeps its shape, and the disk runs off toward the words on
    its own. An earlier version mirrored the edge to fill the gap and produced
    a second, ghostly hole, which is exactly the kind of thing a reader notices
    even when they cannot say why.
    """
    width, height = size

    scale = RENDER_WIDTH / source.width
    scaled = source.resize(
        (RENDER_WIDTH, round(source.height * scale)), Image.LANCZOS
    )

    # A band through the middle, where the hole and the disk are, sat against
    # the right edge so the hole lands right of centre.
    #
    # It does not reach the left edge, and it does not need to: the fade below
    # is already near solid black by the time it gets there, so the join is
    # buried rather than patched over.
    top = (scaled.height - height) // 2
    band = scaled.crop((0, top, RENDER_WIDTH, top + height))

    canvas = Image.new("RGB", size, (0, 0, 0))
    canvas.paste(band, (width - RENDER_WIDTH, 0))

    # Fade the left into black so the words sit on a quiet ground. The fade is
    # eased rather than linear, so it clears the text and then gets out of the
    # way quickly instead of dimming the disk all the way across.
    shade = Image.new("L", (TEXT_ZONE, height), 0)
    pixels = shade.load()
    for column in range(TEXT_ZONE):
        fraction = column / TEXT_ZONE
        pixels_value = int(255 * max(0.0, 1.0 - fraction**2.2))
        for row in range(height):
            pixels[column, row] = pixels_value

    canvas.paste(Image.new("RGB", (TEXT_ZONE, height), (0, 0, 0)), (0, 0), shade)
    return canvas


def build_banner(source: Image.Image) -> Image.Image:
    banner = widen(source, BANNER)
    draw = ImageDraw.Draw(banner)

    # Sized to the word, not to a number picked once.
    #
    # "Singularity" is nearly three times the length of the name it replaced,
    # and at the old size it ran straight into the disk. The title shrinks
    # until it fits the space left of the hole, so renaming the project again
    # will not silently break the picture.
    title = None
    for size in range(84, 39, -2):
        title = load_font(size, bold=True)
        left, _, right, _ = draw.textbbox((0, 0), "Singularity", font=title)
        if right - left <= TEXT_ZONE - 150:
            break

    body = load_font(22)
    chip = load_font(14, bold=True)

    draw.text((96, 156), "Singularity", font=title, fill=TEXT)
    draw.text((100, 268), "A Discord client in C++,", font=body, fill=MUTED)
    draw.text((100, 298), "built from scratch.", font=body, fill=MUTED)

    x = 100
    for label in ("C++20", "Qt 6", "DAVE E2EE"):
        left, top, right, bottom = draw.textbbox((0, 0), label, font=chip)
        width = right - left + 26
        draw.rounded_rectangle(
            (x, 344, x + width, 374), radius=15, outline=ACCENT + (110,), width=1
        )
        draw.text(
            (x + 13, 359 - (bottom - top) // 2 - top), label, font=chip, fill=ACCENT
        )
        x += width + 12

    return banner


def build_mark(source: Image.Image) -> Image.Image:
    """A square crop, tight on the hole, for use as an icon."""
    side = min(source.size)
    left = (source.width - side) // 2
    top = (source.height - side) // 2
    square = source.crop((left, top, left + side, top + side))
    return square.resize((MARK, MARK), Image.LANCZOS)


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: make_banner.py <source image>")
        return 1

    path = Path(sys.argv[1])
    if not path.exists():
        print(f"no such file: {path}")
        return 1

    source = Image.open(path).convert("RGB")
    BRAND.mkdir(exist_ok=True)

    build_banner(source).save(BRAND / "banner.png", optimize=True)
    build_mark(source).save(BRAND / "mark.png", optimize=True)

    print(f"wrote {BRAND / 'banner.png'}")
    print(f"wrote {BRAND / 'mark.png'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
