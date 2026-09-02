#!/usr/bin/env python3
"""Процедурная текстура деревянного ящика -> crate.png (одна грань куба, UV 0..1).

Доски (вертикальные, с прожилками и швами) + рама по краю (со скосом) + металлические
уголки с заклёпками + лёгкое затемнение краёв для объёма. Детерминированно (seed).

Запуск: python gen_crate.py   (кладёт crate.png рядом со скриптом)
Нужен Pillow (+ numpy). Перегенерировать при смене вида ящика.
"""
import math
import os
import random

import numpy as np
from PIL import Image, ImageDraw

S = 512                    # сторона текстуры
SEED = 42
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "crate.png")

random.seed(SEED)
np.random.seed(SEED)

# --- Палитра (тёплое дерево + тёмная сталь) ---
WOOD = np.array([150.0, 102.0, 56.0])   # базовый цвет доски
GROOVE = 0.30                            # доля яркости в шве между досками (тёмный паз)
FRAME = (120, 80, 42)                    # рама (чуть темнее/краснее доски)
FRAME_HI = (150, 104, 60)               # верхняя фаска рамы (свет)
FRAME_LO = (86, 56, 28)                 # нижняя фаска рамы (тень)
GROOVE_LINE = (58, 37, 18)
METAL = (80, 83, 90)
METAL_D = (48, 50, 58)
RIVET = (150, 153, 162)


def wood_base():
    """Вертикальные доски с потоновой вариацией, швами и горизонтальной прожилкой."""
    img = np.zeros((S, S, 3), dtype=np.float32)
    planks = 5
    pw = S / planks
    # Горизонтальная прожилка (общая по строке): слоёный синус + шум.
    grain = np.ones(S, dtype=np.float32)
    for y in range(S):
        g = (1.0 + 0.06 * math.sin(y * 0.09) + 0.04 * math.sin(y * 0.023 + 1.7)
             + 0.05 * (np.random.rand() - 0.5))
        grain[y] = g
    for x in range(S):
        pi = int(x / pw)
        rng = random.Random(1000 + pi)
        tone = 0.82 + 0.30 * rng.random()          # своя яркость у каждой доски
        col = WOOD * tone
        local = (x % pw) / pw
        edge = min(local, 1.0 - local)
        groove = min(edge / 0.055, 1.0)            # 0 в шве -> 1 в теле доски
        shade = GROOVE + (1.0 - GROOVE) * groove
        img[:, x] = col * shade * grain[:, None]
    img += (np.random.rand(S, S, 1) - 0.5) * 10.0  # мелкое зерно
    return img


def add_edge_darken(img):
    """Лёгкое затемнение к краям грани — объём."""
    yy, xx = np.mgrid[0:S, 0:S].astype(np.float32)
    dx = np.minimum(xx, S - 1 - xx) / (S * 0.5)
    dy = np.minimum(yy, S - 1 - yy) / (S * 0.5)
    edge = np.clip(np.minimum(dx, dy) / 0.14, 0.0, 1.0)
    v = (0.72 + 0.28 * edge)[:, :, None]
    return img * v


def main():
    img = add_edge_darken(wood_base())
    im = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), "RGB")
    d = ImageDraw.Draw(im)

    fw = int(S * 0.11)                              # ширина рамы
    b = int(S * 0.02)                               # ширина фаски
    # Рамы-рейки: верх, низ, лево, право.
    for box in ([0, 0, S, fw], [0, S - fw, S, S], [0, 0, fw, S], [S - fw, 0, S, S]):
        d.rectangle(box, fill=FRAME)
    # Фаски рамы (свет сверху-слева, тень снизу-справа) — псевдо-объём.
    d.rectangle([0, 0, S, b], fill=FRAME_HI)
    d.rectangle([0, 0, b, S], fill=FRAME_HI)
    d.rectangle([0, S - b, S, S], fill=FRAME_LO)
    d.rectangle([S - b, 0, S, S], fill=FRAME_LO)
    # Внутренний паз между рамой и досками.
    d.rectangle([fw, fw, S - fw, S - fw], outline=GROOVE_LINE, width=3)

    # Металлические уголки с заклёпками.
    mb = int(S * 0.17)
    r = max(2, int(S * 0.013))
    for (cx, cy) in [(0, 0), (S - mb, 0), (0, S - mb), (S - mb, S - mb)]:
        d.rectangle([cx, cy, cx + mb, cy + mb], fill=METAL, outline=METAL_D, width=2)
        for fx in (0.26, 0.74):
            for fy in (0.26, 0.74):
                rx, ry = cx + mb * fx, cy + mb * fy
                d.ellipse([rx - r, ry - r, rx + r, ry + r], fill=RIVET, outline=METAL_D)

    im.save(OUT)
    print("crate.png сохранён:", OUT, im.size)


if __name__ == "__main__":
    main()
