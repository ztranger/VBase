#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Бесшовные тайл-текстуры земли для пола сцен: grass.png (трава) и dirt.png (утоптанная земля).
Шум тороидальный (враппится по краям) -> тайлится без швов. Запуск: python gen_ground.py"""
import os, random
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
SIZE = 256

def smooth(t): return t * t * (3.0 - 2.0 * t)

def tile_grid(cells, seed):
    rng = random.Random(seed)
    return [[rng.random() for _ in range(cells)] for _ in range(cells)]

def sample(grid, cells, u, v):
    x = u * cells; y = v * cells
    x0 = int(x) % cells; y0 = int(y) % cells
    x1 = (x0 + 1) % cells; y1 = (y0 + 1) % cells
    fx = smooth(x - int(x)); fy = smooth(y - int(y))
    a = grid[y0][x0]; b = grid[y0][x1]; c = grid[y1][x0]; d = grid[y1][x1]
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy

def fbm(octaves, base_cells, seed):
    grids = [(base_cells * (2 ** o), tile_grid(base_cells * (2 ** o), seed + o * 97)) for o in range(octaves)]
    out = [[0.0] * SIZE for _ in range(SIZE)]
    amp_sum = 0.0
    for o, (cells, g) in enumerate(grids):
        amp = 0.5 ** o; amp_sum += amp
        for j in range(SIZE):
            v = j / SIZE
            for i in range(SIZE):
                out[j][i] += amp * sample(g, cells, i / SIZE, v)
    for j in range(SIZE):
        for i in range(SIZE):
            out[j][i] /= amp_sum
    return out

def lerp(a, b, t): return tuple(int(a[k] + (b[k] - a[k]) * t) for k in range(3))

def make(path, seed, lo, hi, speck, speck_col, speck_p):
    n = fbm(4, 4, seed)          # крупные пятна + мелкая деталь
    d = fbm(3, 16, seed + 1000)  # высокочастотная крапинка
    rng = random.Random(seed + 7)
    img = Image.new("RGB", (SIZE, SIZE))
    px = img.load()
    for j in range(SIZE):
        for i in range(SIZE):
            t = max(0.0, min(1.0, n[j][i] * 0.85 + d[j][i] * 0.15))
            col = lerp(lo, hi, t)
            if speck and rng.random() < speck_p:  # редкие вкрапления (камешки/цветы)
                col = speck_col
            px[i, j] = col
    img.save(path)
    print("wrote", path)

def make_normal(path, seed, strength):
    """Органичная бесшовная tangent-space нормал-карта из шума высоты: R=наклон X, G=наклон Z,
    B=вверх. Даёт «фейковый» рельеф земли через освещение (геометрия остаётся плоской).
    strength — крутизна: градиент высоты по соседним пикселям крошечный (~0.005), поэтому нужен
    большой множитель (десятки), иначе нормали почти плоские и эффекта не видно."""
    h = fbm(4, 4, seed)  # тайловая высота (крупные комья + деталь)
    img = Image.new("RGB", (SIZE, SIZE))
    px = img.load()
    devs = []
    for j in range(SIZE):
        for i in range(SIZE):
            hl = h[j][(i - 1) % SIZE]; hr = h[j][(i + 1) % SIZE]      # градиент по X (wrap = бесшовно)
            hu = h[(j - 1) % SIZE][i]; hd = h[(j + 1) % SIZE][i]      # градиент по Z
            nx = -(hr - hl) * strength
            nz = -(hd - hu) * strength
            ny = 1.0
            l = (nx * nx + ny * ny + nz * nz) ** 0.5
            r = nx / l; g = nz / l
            devs.append(abs(r)); devs.append(abs(g))
            px[i, j] = (int((r * 0.5 + 0.5) * 255),   # R -> наклон вдоль тангента (+X)
                        int((g * 0.5 + 0.5) * 255),   # G -> наклон вдоль бинормали (Z)
                        int((ny / l * 0.5 + 0.5) * 255))   # B -> нормаль вверх (~1)
    img.save(path)
    avg = sum(devs) / len(devs)
    print(f"wrote {path}  avg|tilt|={avg:.3f} (цель ~0.25-0.5 для заметного рельефа)")

# Трава: тёмно- -> светло-зелёная, редкие жёлто-зелёные пятнышки.
make(os.path.join(HERE, "grass.png"), seed=42,
     lo=(46, 78, 40), hi=(96, 132, 62), speck=True, speck_col=(120, 140, 70), speck_p=0.010)
# Земля/тропа: тёмно- -> светло-коричневая, редкие светлые камешки.
make(os.path.join(HERE, "dirt.png"), seed=91,
     lo=(74, 56, 38), hi=(120, 96, 66), speck=True, speck_col=(140, 128, 104), speck_p=0.012)
# Нормал-карта земли (лёгкий «фейковый» рельеф через свет). strength большой — градиент по
# пикселям крошечный; подобран так, чтобы avg|tilt| был ~0.3 (заметно, но не карикатурно).
make_normal(os.path.join(HERE, "ground_n.png"), seed=205, strength=70.0)
