"""Renders the DisplayMaster mark (laptop screen + tablet) to .ico/.png assets.

Same geometry as the Android AppLogo / adaptive icon so both apps share one mark.
Usage: python tools/make_icons.py
"""
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
ACCENT = (59, 107, 255)       # #3B6BFF
CONNECTED = (0, 191, 165)     # #00BFA5
BACK = (143, 168, 255, 150)   # translucent accent-light outline
INK = (11, 13, 18, 255)       # #0B0D12


def gradient(size, c0, c1):
    """Diagonal linear gradient image."""
    w, h = size
    img = Image.new("RGBA", size)
    px = img.load()
    for y in range(h):
        for x in range(w):
            t = (x + y) / max(1, (w + h - 2))
            px[x, y] = tuple(int(c0[i] + (c1[i] - c0[i]) * t) for i in range(3)) + (255,)
    return img


def render(size: int, background: bool) -> Image.Image:
    ss = 4  # supersample for smooth edges
    s = size * ss
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    if background:
        d.rounded_rectangle([0, 0, s - 1, s - 1], radius=int(s * 0.22), fill=INK)
        pad = 0.14
    else:
        pad = 0.0
    k = 1 - 2 * pad

    def box(x, y, w, h):
        return [s * (pad + x * k), s * (pad + y * k), s * (pad + (x + w) * k), s * (pad + (y + h) * k)]

    # Back: laptop screen outline.
    d.rounded_rectangle(box(0.06, 0.14, 0.62, 0.44), radius=int(s * 0.08 * k),
                        outline=BACK, width=max(1, int(s * 0.075 * k)))
    # Front: tablet with gradient fill.
    front = box(0.36, 0.38, 0.58, 0.48)
    fw, fh = int(front[2] - front[0]), int(front[3] - front[1])
    mask = Image.new("L", (fw, fh), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, fw - 1, fh - 1], radius=int(s * 0.09 * k), fill=255)
    img.paste(gradient((fw, fh), ACCENT, CONNECTED), (int(front[0]), int(front[1])), mask)
    return img.resize((size, size), Image.LANCZOS)


def main():
    assets = ROOT / "windows" / "ui" / "DisplayMaster.App" / "Assets"
    assets.mkdir(parents=True, exist_ok=True)
    sizes = [16, 20, 24, 32, 40, 48, 64, 256]
    # App icon: mark on the dark rounded tile. Tray icon: bare mark (small sizes read better).
    render(256, True).save(assets / "AppIcon.ico", sizes=[(n, n) for n in sizes])
    render(256, False).save(assets / "TrayIcon.ico", sizes=[(n, n) for n in [16, 20, 24, 32, 48]])
    render(128, False).save(assets / "Logo.png")
    print("wrote", assets)


if __name__ == "__main__":
    main()
