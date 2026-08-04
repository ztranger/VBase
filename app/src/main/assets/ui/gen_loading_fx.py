"""Soft glow/beam overlays for loading preview (cinematic, not blob spam)."""
from pathlib import Path
import math

from PIL import Image

OUT = Path(__file__).resolve().parent


def make_glow(size: int = 256) -> None:
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    px = img.load()
    c = (size - 1) * 0.5
    for y in range(size):
        for x in range(size):
            dx = (x - c) / c
            dy = (y - c) / c
            d = math.sqrt(dx * dx + dy * dy)
            # Широкий мягкий гаусс — без жёсткого диска.
            a = math.exp(-d * d * 2.8)
            a = max(0.0, min(1.0, a))
            # Тёплый янтарный в RGB, яркость только через alpha.
            px[x, y] = (255, 190, 110, int(a * 255))
    img.save(OUT / "loading_glow.png")
    print("wrote loading_glow.png")


def make_beam(w: int = 96, h: int = 320) -> None:
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    px = img.load()
    cx = (w - 1) * 0.5
    for y in range(h):
        # Сверху ярче (у ядра), вниз гаснет.
        along = y / max(h - 1, 1)
        fall_y = math.pow(1.0 - along, 1.35)
        for x in range(w):
            # Горизонтальный гаусс: узкий у верха, чуть шире внизу.
            half = (0.22 + along * 0.35) * w * 0.5
            dx = abs(x - cx) / max(half, 1.0)
            fall_x = math.exp(-dx * dx * 2.2)
            a = fall_x * fall_y
            if a < 0.004:
                continue
            # Чуть белее в центре жилы.
            core = math.exp(-dx * dx * 6.0)
            r = int(255)
            g = int(170 + 50 * core)
            b = int(70 + 40 * core)
            px[x, y] = (r, g, b, int(min(1.0, a) * 220))
    img.save(OUT / "loading_beam.png")
    print("wrote loading_beam.png")


def make_mote(size: int = 32) -> None:
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    px = img.load()
    c = (size - 1) * 0.5
    for y in range(size):
        for x in range(size):
            dx = (x - c) / c
            dy = (y - c) / c
            d = math.sqrt(dx * dx + dy * dy)
            a = math.exp(-d * d * 5.5)
            px[x, y] = (255, 235, 190, int(max(0.0, min(1.0, a)) * 255))
    img.save(OUT / "loading_mote.png")
    print("wrote loading_mote.png")


if __name__ == "__main__":
    make_glow()
    make_beam()
    make_mote()
    print("done")
