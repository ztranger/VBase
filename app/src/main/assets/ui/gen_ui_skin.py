"""Generate UiSkin PNG assets (9-slice panel + button states)."""
from pathlib import Path

from PIL import Image, ImageDraw

OUT = Path(__file__).resolve().parent


def lerp(a: int, b: int, t: float) -> int:
    return int(a + (b - a) * t)


def make_panel(size: int = 128, border: int = 32) -> None:
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    px = img.load()
    rim = (110, 175, 255, 255)
    frame_hi = (48, 78, 130, 255)
    frame_lo = (22, 36, 62, 255)
    fill = (10, 14, 26, 220)

    for y in range(size):
        for x in range(size):
            edge = x < border or y < border or x >= size - border or y >= size - border
            if not edge:
                px[x, y] = fill
                continue

            d = min(x, y, size - 1 - x, size - 1 - y)
            t = d / max(border - 1, 1)
            if d < 2 or d >= border - 2:
                px[x, y] = rim
            else:
                c = (
                    lerp(frame_hi[0], frame_lo[0], t),
                    lerp(frame_hi[1], frame_lo[1], t),
                    lerp(frame_hi[2], frame_lo[2], t),
                    255,
                )
                hl = (x + y) / (2 * size)
                px[x, y] = (
                    min(255, int(c[0] + 18 * hl)),
                    min(255, int(c[1] + 22 * hl)),
                    min(255, int(c[2] + 30 * hl)),
                    255,
                )

    draw = ImageDraw.Draw(img)
    for cx, cy in (
        (border // 2, border // 2),
        (size - border // 2 - 1, border // 2),
        (border // 2, size - border // 2 - 1),
        (size - border // 2 - 1, size - border // 2 - 1),
    ):
        r = 5
        draw.ellipse([cx - r, cy - r, cx + r, cy + r], outline=rim, width=2)

    path = OUT / "panel.png"
    img.save(path)
    print("wrote", path)


def rounded_rect_mask(w: int, h: int, radius: int) -> Image.Image:
    m = Image.new("L", (w, h), 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, w - 1, h - 1], radius=radius, fill=255)
    return m


def make_button(name: str, base_rgb: tuple[int, int, int], w: int = 128, h: int = 48,
                radius: int = 12) -> None:
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    grad = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    gp = grad.load()
    r0, g0, b0 = base_rgb
    for y in range(h):
        t = y / max(h - 1, 1)
        shade = 1.18 - 0.36 * t
        r = min(255, int(r0 * shade))
        g = min(255, int(g0 * shade))
        b = min(255, int(b0 * shade))
        for x in range(w):
            gp[x, y] = (r, g, b, 255)

    mask = rounded_rect_mask(w, h, radius)
    img = Image.composite(grad, img, mask)

    spec = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ImageDraw.Draw(spec).rounded_rectangle(
        [4, 3, w - 5, h // 2], radius=max(1, radius - 2), fill=(255, 255, 255, 55)
    )
    spec_a = Image.composite(spec.split()[-1], Image.new("L", (w, h), 0), mask)
    spec.putalpha(spec_a)
    img = Image.alpha_composite(img, spec)

    border = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    bd = ImageDraw.Draw(border)
    rim = (min(255, r0 + 90), min(255, g0 + 90), min(255, b0 + 110), 230)
    bd.rounded_rectangle([1, 1, w - 2, h - 2], radius=radius, outline=rim, width=2)
    bd.rounded_rectangle(
        [3, 3, w - 4, h - 4], radius=max(1, radius - 2), outline=(0, 0, 0, 70), width=1
    )
    img = Image.alpha_composite(img, border)

    path = OUT / name
    img.save(path)
    print("wrote", path, base_rgb)


if __name__ == "__main__":
    make_panel(128, 32)
    make_button("button_normal.png", (45, 110, 190))
    make_button("button_hover.png", (70, 150, 235))
    make_button("button_active.png", (28, 75, 145))
    print("done")
