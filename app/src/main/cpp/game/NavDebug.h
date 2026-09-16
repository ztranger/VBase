#pragma once

#include <vector>

#include "engine/core/MathUtil.h"
#include "game/FlowField.h"  // NavCell
#include "game/Grid.h"

struct ColliderSpec;  // game/SceneDesc.h (статичная геометрия сцены)

// Отладочный снимок навигации: клетка сетки с занятостью и полем потока.
struct NavDebugCell {
    int cx = 0, cz = 0;       // индексы клетки
    bool blocked = false;     // занята препятствием (obstacle)
    bool reachable = false;   // из клетки есть путь к цели
    bool goal = false;        // клетка цели (ядро)
    float dirX = 0.0f, dirZ = 0.0f;  // направление потока (горизонт, нормализовано; 0 = нет)
    int dist = -1;            // BFS-дистанция до ближайшей цели; -1 = недостижимо
};

// Прямоугольный блокатор (футпринт здания) для реконструкции навсетки на клиенте.
struct NavDebugBlocker {
    Vec3 center{0.0f, 0.0f, 0.0f};
    Vec3 half{0.0f, 0.0f, 0.0f};
};

// Плоский снимок навсетки + поля потока для отладочного оверлея. Клиент реконструирует
// его ТЕМИ ЖЕ NavGrid/FlowField, что и авторитетный сервер (GameWorld::rebuildNavIfNeeded /
// ensureFlowField) — из коллайдеров сцены, футпринтов блокирующих зданий и клеток целей.
// Чистые данные (game/), без рендера/ImGui: рисует оверлей в engine/render поверх кадра.
struct NavDebugFrame {
    bool valid = false;
    float cell = 2.0f;
    int minX = 0, minZ = 0, w = 0, h = 0;  // начало (в индексах клеток) и размеры карты
    int blockedCount = 0;
    int goalCount = 0;
    int reachableCount = 0;
    int maxDist = 0;                   // максимальная достижимая дистанция (для хитмапа)
    std::vector<NavDebugCell> cells;   // w*h, row-major: (cz-minZ)*w + (cx-minX)

    const NavDebugCell* at(int cx, int cz) const;
};

// Собрать снимок: colliders — статика сцены (растеризуются с запасом под радиус агента,
// как на сервере), blockers — футпринты блокирующих зданий (без запаса), goals — клетки
// целей (живые ядра). agentClearance должен совпадать с серверным kEnemyRadius.
NavDebugFrame buildNavDebug(const Grid& grid,
                            const std::vector<ColliderSpec>& colliders,
                            const std::vector<NavDebugBlocker>& blockers,
                            const std::vector<NavCell>& goals,
                            float agentClearance);
