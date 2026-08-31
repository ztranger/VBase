# Генератор набора сцен-сценариев VBase + манифест scenes/scenes.cfg.
# Стены/колонны = визуальные ящики + совпадающие коллайдеры (постоянные, не ломаются).
# Здания (core/spawner/tower/generator/storage) = сущности сервера (footprint в навсетке).
import io, os

# Пути — относительно репозитория (tools/ лежит в корне): переносимо между машинами.
_REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(_REPO, "app", "src", "main", "assets")
SCENES = os.path.join(ASSETS, "scenes")

CELL = 2.0
def cc(c):  # центр клетки c (world) для cell=2 -> нечётные координаты
    return int((c + 0.5) * CELL)

def header(w, arena, cam_dist, cam_pitch, title_lines):
    for L in title_lines:
        w(f"# {L}\n")
    w("\n# --- Текстуры / материалы / меши ---\n")
    w("texture checker procedural 256 8\n")
    w("texture crate   image textures/crate.png\n")
    w("material floor lit   color 1 1 1          tex checker\n")
    w("material crate lit   color 1 1 1          tex crate\n")
    w("material edge  lit   color 0.55 0.65 0.85 tex checker\n")
    w(f"mesh ground plane {arena*2} {arena}\n")
    w("mesh box    cube  1\n\n")
    w("# --- Свет / камера / сетка ---\n")
    w("light  dir 0.4 1 0.6\n")
    w(f"camera distance {cam_dist} pitch {cam_pitch}\n")
    w(f"grid   cell 2 arena {arena-2}\n")
    w("matchrestart 8\n\n")
    w("object ground mat floor\n")
    w(f"ring box mat edge count 64 radius {arena-1} y 0.4 scale 0.4 spin 0.6\n\n")
    w("# --- Границы арены (коллайдеры) ---\n")
    w(f"collider box center 0 -0.5 0  half {arena} 0.5 {arena}   # пол\n")
    w(f"collider box center  {arena} 1 0  half 0.25 1 {arena}   # стена +X\n")
    w(f"collider box center -{arena} 1 0  half 0.25 1 {arena}   # стена -X\n")
    w(f"collider box center 0 1  {arena}  half {arena} 1 0.25   # стена +Z\n")
    w(f"collider box center 0 1 -{arena}  half {arena} 1 0.25   # стена -Z\n\n")

def crate(w, X, Z):
    w(f"object box mat crate pos {X} 0.9 {Z} scale 1.8\n")
    w(f"collider box center {X} 0.9 {Z} half 0.9 0.9 0.9\n")

def player(w, X, Z):
    w(f"\n# --- Игрок ---\nplayer model models/Mage.glb pos {X} 0 {Z} scale 1.0 hide wand,spellbook\n")

# ---------- 1) Открытая арена (базовая) ----------
def scene_open():
    s = io.StringIO(); w = s.write
    header(w, 16, 30, 0.95, [
        "Открытая арена VBase (без препятствий). Базовое поведение: прямой подход,",
        "сглаженный доворот. Сравнение с лабиринтами."])
    w("# --- База (запад) ---\n")
    w(f"core      pos {cc(-6)} 0 0\n")
    w(f"generator pos {cc(-7)} 0 4\n")
    w(f"storage   pos {cc(-7)} 0 -4\n")
    w(f"tower     pos {cc(-4)} 0 4\n")
    w(f"tower     pos {cc(-4)} 0 -4\n\n")
    w("# --- Спавнеры (восток) ---\n")
    w(f"spawner   pos {cc(6)} 0 6\n")
    w(f"spawner   pos {cc(6)} 0 -6\n")
    player(w, cc(-6), 8)
    return s.getvalue()

# ---------- 2) Лабиринт: серпантин ----------
def scene_maze():
    s = io.StringIO(); w = s.write
    header(w, 24, 40, 1.05, [
        "Лабиринт-серпантин. Спавнеры на востоке, база на западе. Мобы вьются через",
        "4 стены с попеременными проходами (сверху/снизу). Рашеры -> к ядру, миньоны -> ломать."])
    walls = [(12, {18,20,22}), (4, {-22,-20,-18}), (-4, {18,20,22}), (-12, {-22,-20,-18})]
    for name,(X,gap) in zip("ABCD", walls):
        w(f"# Стена {name} (X={X}), проход {'сверху' if 22 in gap else 'снизу'}\n")
        for Z in range(-22, 23, 2):
            if Z in gap: continue
            crate(w, X, Z)
        w("\n")
    w("# --- База (запад) ---\n")
    w("core      pos -20 0 0\n")
    w("generator pos -22 0 6\n")
    w("storage   pos -22 0 -6\n")
    w("tower     pos -18 0 5\n")
    w("tower     pos -18 0 -5\n\n")
    w("# --- Спавнеры (восток) ---\n")
    w("spawner   pos 20 0 8\n")
    w("spawner   pos 20 0 -8\n")
    player(w, -20, 10)
    return s.getvalue()

# ---------- 3) Лес колонн (решётка) ----------
def scene_pillars():
    s = io.StringIO(); w = s.write
    header(w, 22, 40, 1.05, [
        "Лес колонн: решётка одиночных препятствий с коридорами в 1 клетку.",
        "Мобы петляют между колонн к ядру. Проверка потока в плотной решётке."])
    w("# --- Колонны: клетки с чётными cx,cz в центральной зоне ---\n")
    for cx in range(-7, 8):
        for cz in range(-7, 8):
            if cx % 2 == 0 and cz % 2 == 0:
                crate(w, cc(cx), cc(cz))
    w("\n# --- База (запад) ---\n")
    w(f"core      pos {cc(-9)} 0 0\n")
    w(f"generator pos {cc(-10)} 0 4\n")
    w(f"storage   pos {cc(-10)} 0 -4\n")
    w(f"tower     pos {cc(-9)} 0 6\n")
    w(f"tower     pos {cc(-9)} 0 -6\n\n")
    w("# --- Спавнеры (восток) ---\n")
    w(f"spawner   pos {cc(9)} 0 6\n")
    w(f"spawner   pos {cc(9)} 0 -6\n")
    player(w, cc(-9), 8)
    return s.getvalue()

# ---------- 4) Запечатанное ядро (прогрыз) ----------
def scene_sealed():
    s = io.StringIO(); w = s.write
    header(w, 16, 30, 1.05, [
        "Запечатанное ядро: кольцо построек наглухо окружает ядро (клетки-соседи).",
        "Пути к ядру нет -> рашеры ЛОМАЮТ кольцо и прорываются. Демонстрация отказа от",
        "костыля с ограничением построек."])
    w("# --- Ядро и кольцо (генераторы/хранилища = блокеры, не стреляют, ломаемы) ---\n")
    w(f"core      pos {cc(0)} 0 {cc(0)}\n")
    ring = [(-1,-1),(-1,0),(-1,1),(0,-1),(0,1),(1,-1),(1,0),(1,1)]
    for i,(dx,dz) in enumerate(ring):
        kind = "generator" if (i % 2 == 0) else "storage"
        w(f"{kind} pos {cc(dx)} 0 {cc(dz)}\n")
    w("\n# --- Спавнеры (по кругу снаружи) ---\n")
    w(f"spawner   pos {cc(5)} 0 {cc(0)}\n")
    w(f"spawner   pos {cc(-5)} 0 {cc(4)}\n")
    player(w, cc(0), cc(5))
    return s.getvalue()

# ---------- 5) Концентрические кольца (спираль внутрь) ----------
def scene_rings():
    s = io.StringIO(); w = s.write
    header(w, 24, 42, 1.1, [
        "Концентрические кольца с проходами на разных сторонах: мобы закручиваются",
        "к центру по спирали. Ядро в центре, спавнеры снаружи."])
    # кольца на чебышёвском радиусе R; проход = 3 клетки на выбранной стороне
    rings = [(9, "E"), (6, "S"), (3, "E")]  # чередуем сторону прохода
    w("# --- Кольца ---\n")
    for R, side in rings:
        for c in range(-R, R + 1):
            for (cx, cz) in {(c, -R), (c, R), (-R, c), (R, c)}:
                # проход: 3 клетки по центру нужной стороны
                if side == "E" and cx == R and cz in (-1, 0, 1): continue
                if side == "W" and cx == -R and cz in (-1, 0, 1): continue
                if side == "S" and cz == -R and cx in (-1, 0, 1): continue
                if side == "N" and cz == R and cx in (-1, 0, 1): continue
                crate(w, cc(cx), cc(cz))
    w("\n# --- Ядро в центре, спавнеры снаружи ---\n")
    w(f"core      pos {cc(0)} 0 {cc(0)}\n")
    w(f"tower     pos {cc(1)} 0 {cc(1)}\n")
    w(f"spawner   pos {cc(11)} 0 {cc(0)}\n")
    w(f"spawner   pos {cc(0)} 0 {cc(-11)}\n")
    player(w, cc(0), cc(2))
    return s.getvalue()

# ---------- 6) DFS-лабиринт (идеальный: единственный путь) ----------
def scene_dfs(M, seed, cam):
    import random
    rnd = random.Random(seed)
    N = 3 * M + 1                       # клеток навсетки на сторону (комната=2, стена=1)
    wall = [[True] * N for _ in range(N)]
    def room_cells(i, j):
        return [(i * 3 + 1 + dx, j * 3 + 1 + dy) for dx in (0, 1) for dy in (0, 1)]
    visited = [[False] * M for _ in range(M)]
    def carve(i, j):
        visited[j][i] = True
        for (x, y) in room_cells(i, j):
            wall[y][x] = False
        dirs = [(1, 0), (-1, 0), (0, 1), (0, -1)]
        rnd.shuffle(dirs)
        for di, dj in dirs:
            ni, nj = i + di, j + dj
            if 0 <= ni < M and 0 <= nj < M and not visited[nj][ni]:
                if di != 0:            # проём по X: 2 клетки высотой комнаты
                    wx = min(i, ni) * 3 + 3
                    for dy in (0, 1):
                        wall[j * 3 + 1 + dy][wx] = False
                else:                  # проём по Z
                    wy = min(j, nj) * 3 + 3
                    for dx in (0, 1):
                        wall[wy][i * 3 + 1 + dx] = False
                carve(ni, nj)
    carve(0, 0)

    off = N // 2                       # центрируем навсетку на (0,0)
    arena = off * 2 + 3
    s = io.StringIO(); w = s.write
    header(w, arena, cam, 1.15, [
        f"DFS-лабиринт {M}x{M} комнат (seed {seed}): идеальный лабиринт — ровно один путь",
        "от спавнера (комната ЮЗ) до ядра (комната СВ). Коридоры в 2 клетки."])
    w("# --- Стены лабиринта ---\n")
    # без внешнего кольца (его заменяют граничные коллайдеры header'а)
    for y in range(N):
        for x in range(N):
            if x == 0 or y == 0 or x == N - 1 or y == N - 1:
                continue
            if wall[y][x]:
                crate(w, cc(x - off), cc(y - off))
    # центр комнаты (i,j) в навсетке: (i*3+1, j*3+1) (+ смещение центра клетки)
    def room_world(i, j):
        return cc(i * 3 + 1 - off), cc(j * 3 + 1 - off)
    sx, sz = room_world(0, 0)
    ex, ez = room_world(M - 1, M - 1)
    w("\n# --- Ядро (комната СВ) и спавнер (комната ЮЗ) ---\n")
    w(f"core      pos {ex} 0 {ez}\n")
    w(f"tower     pos {ex} 0 {ez - 2}\n")
    w(f"spawner   pos {sx} 0 {sz}\n")
    player(w, ex, ez + 2)
    return s.getvalue()

scenes = [
    ("scenes/arena_open.scene", "Открытая арена (база)",        scene_open),
    ("scenes/maze.scene",       "Лабиринт: серпантин",          scene_maze),
    ("scenes/pillars.scene",    "Лес колонн (решётка)",         scene_pillars),
    ("scenes/sealed.scene",     "Запечатанное ядро (прогрыз)",  scene_sealed),
    ("scenes/rings.scene",      "Кольца (спираль внутрь)",      scene_rings),
    ("scenes/maze_dfs_s.scene", "DFS-лабиринт 4x4",             lambda: scene_dfs(4, 1, 34)),
    ("scenes/maze_dfs_l.scene", "DFS-лабиринт 6x6",             lambda: scene_dfs(6, 7, 46)),
]

for path, name, fn in scenes:
    full = os.path.join(ASSETS, path.replace("scenes/", "scenes\\"))
    text = fn()
    with open(full, "w", encoding="utf-8") as f:
        f.write(text)
    print(f"  {path}: {len(text.splitlines())} lines")

# Манифест: default (существует) + сгенерированные.
man = io.StringIO(); w = man.write
w("# Список сцен для выбора в клиенте (меню -> Сцена). Формат строки:\n")
w("#   <путь> <отображаемое имя ...>\n")
w("# Путь читается через AssetSource (работает и на десктопе, и на Android).\n\n")
w("scenes/default.scene    По умолчанию (стена-угол)\n")
for path, name, _ in scenes:
    w(f"{path}    {name}\n")
with open(os.path.join(ASSETS, "config", "scenes.cfg"), "w", encoding="utf-8") as f:
    f.write(man.getvalue())
print("  config/scenes.cfg written")
# убрать устаревший манифест, если остался от прошлой версии генератора
old = os.path.join(SCENES, "scenes.cfg")
if os.path.exists(old):
    os.remove(old)
    print("  removed stray scenes/scenes.cfg")
