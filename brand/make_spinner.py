"""Renders the frames the installer spins while it works.

Inno Setup can show a bitmap and has no way to play an animation, so the
animation is a stack of still frames that the install swaps between. They are
BMP because that is the only format its image control reads, and 24-bit
because its loader ignores an alpha channel - the transparency has to be baked
against the installer's own background or the logo arrives in a square.

Drawn a pixel at a time rather than out of ellipses and lines. The first
attempt used the drawing primitives and came out looking like Saturn: an
outlined ball with a hoop around it. What makes the shape read as a black hole
is not the outline, it is that the disk is a thick band of light which passes
*behind* the shadow on the far side and *in front* of it on the near side.
That ordering cannot be drawn with an ellipse, so each pixel is asked which
part of the picture it belongs to.

Run:
    python brand\\make_spinner.py
"""

import math
import os

from PIL import Image, ImageFilter

BACKGROUND = (7, 9, 14)

# The silver the shipped theme actually produces: Singularity's default seed
# has no hue, so the disk takes the silver branch rather than being tinted.
COOL = (150, 180, 225)
HOT = (255, 255, 255)

SIZE = 96
FRAMES = 24

# Rendered large and shrunk at the end, which is cheaper than working out
# coverage per pixel and gives the same smooth edges.
SUPER = 3

# How flat the disk is seen. Near enough edge on to read as a disk rather than
# a ring, but open enough to show the far side arcing over the top.
SQUASH = 0.26

HOLE = 0.21        # the shadow, in screen radius
RING = 0.225       # the photon ring, hard against it
DISK_IN = 0.30     # the disk's inner edge, measured in the disk's own plane
DISK_OUT = 0.92


def frame(index: int) -> Image.Image:
    side = SIZE * SUPER
    image = Image.new("RGB", (side, side), BACKGROUND)
    pixels = image.load()

    turn = (index / FRAMES) * 2.0 * math.pi
    centre = side / 2.0
    unit = side / 2.0

    for py in range(side):
        # Screen coordinates, -1 to 1.
        sy = (py + 0.5 - centre) / unit
        for px in range(side):
            sx = (px + 0.5 - centre) / unit

            screen_r = math.hypot(sx, sy)
            if screen_r > 1.02:
                continue

            # The same point, measured in the plane the disk lies in. The
            # vertical squash is undone, which turns the ellipse on screen
            # back into the circle it actually is.
            dy = sy / SQUASH
            disk_r = math.hypot(sx, dy)

            light = 0.0
            if DISK_IN < disk_r < DISK_OUT:
                # Thickest just outside the inner edge and thinning outward,
                # which is how an accretion disk actually falls off.
                across = (disk_r - DISK_IN) / (DISK_OUT - DISK_IN)
                body = (1.0 - across) ** 1.4
                body *= min(1.0, across / 0.12)   # soften the inner edge

                # The side turning toward you is brighter. This is the part
                # that moves between frames, and it is the reason the picture
                # reads as spinning rather than as sliding.
                theta = math.atan2(dy, sx)
                toward = 0.5 + 0.5 * math.cos(theta - turn)
                light = body * (0.30 + 0.70 * toward ** 2.0)

            # Which side of the shadow this bit of disk passes. Below the
            # middle of the screen is the near half and is drawn in front;
            # above it is the far half and is hidden by the shadow.
            in_front = sy > 0.0

            if screen_r < HOLE and not in_front:
                light = 0.0

            colour = [0.0, 0.0, 0.0]
            if light > 0.0:
                warmth = min(1.0, light * 1.3)
                for c in range(3):
                    colour[c] = (COOL[c] + (HOT[c] - COOL[c]) * warmth) * min(1.0, light * 1.5)

            # The photon ring: a thin bright circle hugging the shadow. Always
            # visible, because it is light that went round the back and came
            # out toward you.
            edge = abs(screen_r - RING)
            if edge < 0.030:
                glow = (1.0 - edge / 0.030) ** 1.5
                for c in range(3):
                    colour[c] = max(colour[c], HOT[c] * glow)

            if colour[0] <= 0.0 and colour[1] <= 0.0 and colour[2] <= 0.0:
                continue

            pixels[px, py] = tuple(
                min(255, int(BACKGROUND[c] + colour[c])) for c in range(3)
            )

    image = image.filter(ImageFilter.GaussianBlur(SUPER * 0.6))
    return image.resize((SIZE, SIZE), Image.LANCZOS)


def main() -> None:
    here = os.path.dirname(os.path.abspath(__file__))
    out = os.path.join(os.path.dirname(here), "installer", "spinner")
    os.makedirs(out, exist_ok=True)

    for i in range(FRAMES):
        frame(i).save(os.path.join(out, f"spin{i:02d}.bmp"), "BMP")
        print(f"  frame {i + 1}/{FRAMES}", end="\r")

    print(f"\nwrote {FRAMES} frames to {out}")


if __name__ == "__main__":
    main()
