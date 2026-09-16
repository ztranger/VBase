#include "game/NavDebug.h"

#include "game/SceneDesc.h"  // ColliderSpec

const NavDebugCell* NavDebugFrame::at(int cx, int cz) const {
    int lx = cx - minX, lz = cz - minZ;
    if (lx < 0 || lz < 0 || lx >= w || lz >= h) return nullptr;
    return &cells[(size_t)(lz * w + lx)];
}

NavDebugFrame buildNavDebug(const Grid& grid,
                            const std::vector<ColliderSpec>& colliders,
                            const std::vector<NavDebugBlocker>& blockers,
                            const std::vector<NavCell>& goals,
                            float agentClearance) {
    NavDebugFrame f;

    // 1) Занятость — как на сервере: статика с запасом под радиус агента, футпринты без запаса.
    NavGrid map;
    map.reset(grid);
    for (const ColliderSpec& cs : colliders) map.rasterizeBox(cs.center, cs.half, agentClearance);
    for (const NavDebugBlocker& b : blockers) map.rasterizeBox(b.center, b.half);

    // 2) Поле потока к целям (тот же мультиисточниковый BFS, что у мобов).
    FlowField field;
    field.compute(map, goals);

    f.cell = grid.cell;
    f.minX = map.minX();
    f.minZ = map.minZ();
    f.w = map.width();
    f.h = map.height();
    f.goalCount = (int)goals.size();
    if (f.w <= 0 || f.h <= 0) return f;  // valid остаётся false — сетки нет

    f.cells.resize((size_t)f.w * (size_t)f.h);
    for (int lz = 0; lz < f.h; ++lz) {
        for (int lx = 0; lx < f.w; ++lx) {
            const int cx = f.minX + lx, cz = f.minZ + lz;
            NavDebugCell& c = f.cells[(size_t)(lz * f.w + lx)];
            c.cx = cx;
            c.cz = cz;
            c.blocked = map.isBlocked(cx, cz);
            c.reachable = field.reachable(cx, cz);
            c.dist = field.distanceAt(cx, cz);
            const Vec3 d = field.direction(cx, cz);
            c.dirX = d.x;
            c.dirZ = d.z;
            for (const NavCell& g : goals) {
                if (g.x == cx && g.z == cz) { c.goal = true; break; }
            }
            if (c.blocked) ++f.blockedCount;
            if (c.reachable) ++f.reachableCount;
            if (c.dist > f.maxDist) f.maxDist = c.dist;
        }
    }
    f.valid = true;
    return f;
}
