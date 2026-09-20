"""Render the Wisp mark: a lensed photon ring on a transparent field."""

from __future__ import annotations

from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1] / "resources"


def render(size: int) -> Image.Image:
    ss = 8 if size <= 32 else 4
    n = max(size * ss, 64)
    y, x = np.mgrid[0:n, 0:n].astype(np.float64)
    cx = cy = (n - 1) * 0.5
    u = (x - cx) / (n * 0.5)
    v = (y - cy) / (n * 0.5)
    r = np.sqrt(u * u + v * v)

    tilt = np.radians(-18.0)
    ct, st = np.cos(tilt), np.sin(tilt)
    ur = u * ct - v * st
    vr = u * st + v * ct
    disk_r = np.sqrt(ur * ur + (vr / 0.36) ** 2)

    horizon = 0.30
    ring_r = 0.44
    ring_w = 0.055 if size <= 24 else 0.034

    glow = np.exp(-(r * r) / (0.72 * 0.72))
    ring = np.exp(-((r - ring_r) ** 2) / (ring_w * ring_w))
    inner = np.exp(-((r - (ring_r - 0.07)) ** 2) / (0.018 * 0.018)) * 0.55

    disk = np.exp(-((disk_r - 0.78) ** 2) / (0.22 * 0.22))
    disk *= np.clip((r - horizon) / 0.08, 0.0, 1.0)
    # Hide the far side behind the hole; keep a lensed crescent.
    disk *= np.clip(0.18 + 0.95 * np.clip(vr + 0.12, 0.0, 1.0), 0.0, 1.0)
    doppler = 0.35 + 0.65 * np.clip((ur + 1.05) * 0.5, 0.0, 1.0)
    disk *= doppler

    hole = 1.0 / (1.0 + np.exp((r - horizon) * 48.0))

    # RGB: silver-teal photon ring, warmer approaching disk.
    red = (
        0.04 * glow
        + 0.95 * ring
        + 0.35 * inner
        + 1.15 * disk * (0.55 + 0.55 * doppler)
    )
    green = (
        0.07 * glow
        + 1.15 * ring
        + 0.55 * inner
        + 0.85 * disk
    )
    blue = (
        0.10 * glow
        + 1.25 * ring
        + 0.70 * inner
        + 0.55 * disk
    )

    red *= 1.0 - 0.97 * hole
    green *= 1.0 - 0.97 * hole
    blue *= 1.0 - 0.97 * hole

    # Soft circular alpha so the mark is a round badge, not a square tile.
    alpha = np.clip(glow * 1.35 + ring * 1.1 + disk * 0.85, 0.0, 1.0)
    alpha *= np.clip((0.98 - r) / 0.10, 0.0, 1.0)
    alpha = np.maximum(alpha, hole * 0.92 * np.clip((0.92 - r) / 0.08, 0.0, 1.0))

    rgba = np.zeros((n, n, 4), dtype=np.float64)
    rgba[..., 0] = np.clip(red, 0, 1)
    rgba[..., 1] = np.clip(green, 0, 1)
    rgba[..., 2] = np.clip(blue, 0, 1)
    rgba[..., 3] = np.clip(alpha, 0, 1)

    # Premultiply-ish punch-up so 16px still reads as a ring.
    rgb = rgba[..., :3]
    rgb *= 0.55 + 0.45 * rgba[..., 3:4]
    np.clip(rgb, 0, 1, out=rgb)

    img = Image.fromarray((rgba * 255.0 + 0.5).astype(np.uint8), "RGBA")
    if n != size:
        img = img.resize((size, size), Image.Resampling.LANCZOS)
    return img


def main() -> None:
    ROOT.mkdir(parents=True, exist_ok=True)
    master = render(512)
    png_path = ROOT / "wisp.png"
    master.save(png_path, "PNG")

    sizes = [16, 24, 32, 48, 64, 128, 256]
    frames = [render(s) for s in sizes]
    ico_path = ROOT / "wisp.ico"
    frames[-1].save(
        ico_path,
        format="ICO",
        sizes=[(s, s) for s in sizes],
        append_images=frames[:-1],
    )
    print(f"wrote {png_path} {png_path.stat().st_size} bytes")
    print(f"wrote {ico_path} {ico_path.stat().st_size} bytes")


if __name__ == "__main__":
    main()
