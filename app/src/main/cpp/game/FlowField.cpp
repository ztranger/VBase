#include "game/FlowField.h"

#include <algorithm>
#include <cmath>

#include "engine/core/Log.h"

namespace {
constexpr int kDx[8] = {1, -1, 0, 0, 1, 1, -1, -1};
constexpr int kDz[8] = {0, 0, 1, -1, 1, -1, 1, -1};
}  // namespace

void NavGrid::reset(const Grid& grid) {
    grid_ = grid;
    int lo = 0, hi = -1;
    grid_.cellRange(lo, hi);
    // Навигация покрывает клетки, ПЕРЕСЕКАЮЩИЕ арену (не только те, чей центр
    // внутри): здания у края (ядро на |x|=10 при arena=11) иначе выпадают из поля.
    const int edgeLo = grid_.cellOf(-grid_.arenaHalf + 0.01f);
    const int edgeHi = grid_.cellOf(grid_.arenaHalf - 0.01f);
    if (edgeLo < lo) lo = edgeLo;
    if (edgeHi > hi) hi = edgeHi;
    if (lo > hi) {
        minX_ = minZ_ = w_ = h_ = 0;
        blocked_.clear();
        return;
    }
    int side = hi - lo + 1;
    if (side > kMaxSide) {
        LOGW("NavGrid: сторона %d > %d — обрезаем (проверьте grid/arena в сцене)", side,
             kMaxSide);
        // Центрируем обрезку, чтобы (0,0) по возможности остался на карте.
        int extra = side - kMaxSide;
        lo += extra / 2;
        hi = lo + kMaxSide - 1;
        side = kMaxSide;
    }
    minX_ = minZ_ = lo;
    w_ = h_ = side;
    blocked_.assign((size_t)w_ * (size_t)h_, 0);
}

void NavGrid::clearBlocked() {
    std::fill(blocked_.begin(), blocked_.end(), 0);
}

bool NavGrid::inBounds(int cx, int cz) const {
    int lx = cx - minX_, lz = cz - minZ_;
    return lx >= 0 && lz >= 0 && lx < w_ && lz < h_;
}

int NavGrid::index(int cx, int cz) const {
    if (!inBounds(cx, cz)) return -1;
    return (cz - minZ_) * w_ + (cx - minX_);
}

void NavGrid::setBlocked(int cx, int cz, bool blocked) {
    int i = index(cx, cz);
    if (i >= 0) blocked_[(size_t)i] = blocked ? 1 : 0;
}

bool NavGrid::isBlocked(int cx, int cz) const {
    int i = index(cx, cz);
    return i >= 0 && blocked_[(size_t)i] != 0;
}

void NavGrid::rasterizeBox(Vec3 center, Vec3 half, float clearance) {
    // Пол арены (верх на y=0) не должен занять всю карту.
    if (center.y + half.y <= 0.05f) return;
    if (w_ <= 0 || h_ <= 0 || grid_.cell <= 0.0f) return;

    const float cell = grid_.cell;
    const float pad = clearance > 0.0f ? clearance : 0.0f;
    const float minWx = center.x - half.x - pad, maxWx = center.x + half.x + pad;
    const float minWz = center.z - half.z - pad, maxWz = center.z + half.z + pad;
    int cx0 = grid_.cellOf(minWx);
    int cx1 = grid_.cellOf(maxWx - 1e-4f);
    int cz0 = grid_.cellOf(minWz);
    int cz1 = grid_.cellOf(maxWz - 1e-4f);
    if (cx0 > cx1) std::swap(cx0, cx1);
    if (cz0 > cz1) std::swap(cz0, cz1);

    constexpr float kMinOverlap = 0.2f;  // тонкая стена на стыке клетки — не блокируем
    for (int cz = cz0; cz <= cz1; ++cz) {
        float z0 = (float)cz * cell, z1 = z0 + cell;
        float oz = std::min(maxWz, z1) - std::max(minWz, z0);
        if (oz < kMinOverlap) continue;
        for (int cx = cx0; cx <= cx1; ++cx) {
            float x0 = (float)cx * cell, x1 = x0 + cell;
            float ox = std::min(maxWx, x1) - std::max(minWx, x0);
            if (ox < kMinOverlap) continue;
            setBlocked(cx, cz, true);
        }
    }
}

int FlowField::idx(int cx, int cz) const {
    int lx = cx - minX_, lz = cz - minZ_;
    if (lx < 0 || lz < 0 || lx >= w_ || lz >= h_) return -1;
    return lz * w_ + lx;
}

void FlowField::compute(const NavGrid& nav, const std::vector<NavCell>& goals) {
    minX_ = nav.minX();
    minZ_ = nav.minZ();
    w_ = nav.width();
    h_ = nav.height();
    const int n = w_ * h_;
    dist_.assign((size_t)n, kUnreach);
    if (n <= 0) return;

    std::vector<int> q;
    q.reserve((size_t)n);
    for (const NavCell& g : goals) {
        int i = nav.index(g.x, g.z);
        if (i < 0) continue;
        if (dist_[(size_t)i] == 0) continue;  // дубль цели
        dist_[(size_t)i] = 0;
        q.push_back(i);
    }

    // 4 ортогонали, затем 4 диагонали — ортогональный путь выигрывает при равной цене.
    size_t head = 0;
    while (head < q.size()) {
        const int cur = q[head++];
        const int cx = minX_ + (cur % w_);
        const int cz = minZ_ + (cur / w_);
        for (int d = 0; d < 8; ++d) {
            const int nx = cx + kDx[d];
            const int nz = cz + kDz[d];
            const int ni = nav.index(nx, nz);
            if (ni < 0) continue;
            if (dist_[(size_t)ni] != kUnreach) continue;
            if (nav.isBlocked(nx, nz)) continue;  // в занятую не входим (цели уже в очереди)
            if (d >= 4) {  // диагональ: не резать угол сквозь два блока
                if (nav.isBlocked(cx + kDx[d], cz) || nav.isBlocked(cx, cz + kDz[d])) continue;
            }
            dist_[(size_t)ni] = (uint16_t)std::min(65534, (int)dist_[(size_t)cur] + 1);
            q.push_back(ni);
        }
    }
}

bool FlowField::reachable(int cx, int cz) const {
    int i = idx(cx, cz);
    if (i >= 0 && dist_[(size_t)i] != kUnreach) return true;
    // Стоим на блоке (куб/футпринт): путь есть, если соседняя клетка в поле.
    for (int d = 0; d < 8; ++d) {
        int ni = idx(cx + kDx[d], cz + kDz[d]);
        if (ni >= 0 && dist_[(size_t)ni] != kUnreach) return true;
    }
    return false;
}

Vec3 FlowField::direction(int cx, int cz) const {
    int i = idx(cx, cz);
    if (i >= 0 && dist_[(size_t)i] != kUnreach) {
        const uint16_t here = dist_[(size_t)i];
        if (here == 0) return Vec3{0.0f, 0.0f, 0.0f};  // уже на клетке цели
        // Устойчивая база — сосед с наименьшей дистанцией. BFS гарантирует соседа с
        // dist = here-1, поэтому вектор всегда ненулевой и НЕ застревает на «гребнях»,
        // равноудалённых от цели двумя обходами (там градиент был бы нулём).
        int bx = cx, bz = cz;
        uint16_t best = here;
        for (int d = 0; d < 8; ++d) {
            int ni = idx(cx + kDx[d], cz + kDz[d]);
            if (ni < 0 || dist_[(size_t)ni] == kUnreach) continue;
            if (dist_[(size_t)ni] < best) { best = dist_[(size_t)ni]; bx = cx + kDx[d]; bz = cz + kDz[d]; }
        }
        if (bx == cx && bz == cz) return Vec3{0.0f, 0.0f, 0.0f};  // спуска нет (клип dist)
        Vec3 base = normalize(Vec3{(float)(bx - cx), 0.0f, (float)(bz - cz)});
        // Сглаживание: градиент поля по 4 соседям — непрерывный вектор (убирает лесенку).
        // Применяем ТОЛЬКО если он согласован со спуском (dot>0): у вогнутых препятствий
        // градиент может смотреть «в стену», база же всегда ведёт в обход. Стена/за картой
        // читается как (here+1) — вектор отталкивается от препятствия.
        auto sample = [&](int nx, int nz) -> float {
            int ni = idx(nx, nz);
            if (ni < 0 || dist_[(size_t)ni] == kUnreach) return (float)here + 1.0f;
            return (float)dist_[(size_t)ni];
        };
        float gx = sample(cx - 1, cz) - sample(cx + 1, cz);
        float gz = sample(cx, cz - 1) - sample(cx, cz + 1);
        if (gx * gx + gz * gz > 1e-6f) {
            Vec3 g = normalize(Vec3{gx, 0.0f, gz});
            if (g.x * base.x + g.z * base.z > 0.0f) return g;  // согласован — сглаживаем
        }
        return base;
    }
    // Внутри препятствия — шаг на ближайшую (по dist) проходимую соседнюю.
    int bx = 0, bz = 0;
    uint16_t best = kUnreach;
    bool found = false;
    for (int d = 0; d < 8; ++d) {
        const int nx = cx + kDx[d];
        const int nz = cz + kDz[d];
        int ni = idx(nx, nz);
        if (ni < 0 || dist_[(size_t)ni] == kUnreach) continue;
        if (!found || dist_[(size_t)ni] < best) {
            best = dist_[(size_t)ni];
            bx = nx;
            bz = nz;
            found = true;
        }
    }
    if (!found) return Vec3{0.0f, 0.0f, 0.0f};
    Vec3 dir{(float)(bx - cx), 0.0f, (float)(bz - cz)};
    return normalize(dir);
}
