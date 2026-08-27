#pragma once

#include <cstdint>
#include <vector>

#include "engine/core/MathUtil.h"
#include "game/Grid.h"

// Клетка строительной сетки (индексы, не world-координаты).
struct NavCell {
    int x = 0;
    int z = 0;
};

// Занятость клеточной карты. Размер берётся из Grid (арена / размер клетки), не
// зашит: тестовая сцена ~10×10, боевые поля — сотни клеток на сторону.
// Пересчёт поля потока — O(N) от числа клеток; вызывать только при смене занятости.
class NavGrid {
public:
    // Верхняя граница стороны (клеток). 512² ≈ 262k — запас под поля 200–400.
    // Опечатка в сцене с arena на тысячи не должна выделять гигабайты.
    static constexpr int kMaxSide = 512;

    void reset(const Grid& grid);          // перестроить под размеры сетки, всё свободно
    void clearBlocked();
    bool inBounds(int cx, int cz) const;
    int index(int cx, int cz) const;       // -1 если вне карты
    void setBlocked(int cx, int cz, bool blocked);
    bool isBlocked(int cx, int cz) const;
    // Занять клетки, которые пересекает AABB в XZ. Пол (верх ≤ 0) пропускаем.
    // clearance — запас под радиус агента: клетка, куда капсула физически не влезет,
    // не должна считаться проходимой.
    void rasterizeBox(Vec3 center, Vec3 half, float clearance = 0.0f);

    int width() const { return w_; }
    int height() const { return h_; }
    int minX() const { return minX_; }
    int minZ() const { return minZ_; }
    int cellCount() const { return w_ * h_; }
    const Grid& grid() const { return grid_; }

private:
    Grid grid_{};
    int minX_ = 0, minZ_ = 0, w_ = 0, h_ = 0;
    std::vector<uint8_t> blocked_;
};

// Поле потока: мультиисточниковый BFS по NavGrid. Источники — клетки целей
// (ядра / здания). Направление в клетке указывает на родителя (к ближайшей цели
// по длине пути). Недостижимые клетки — нулевой вектор, reachable=false.
class FlowField {
public:
    void compute(const NavGrid& nav, const std::vector<NavCell>& goals);
    // true, если из клетки (или соседней, если стоим внутри блока) есть путь к цели.
    bool reachable(int cx, int cz) const;
    // Горизонтальное направление к цели (нормализованное). (0,0,0) — на цели
    // или путь недоступен. Если текущая клетка занята препятствием — шаг на
    // соседнюю проходимую с минимальной дистанцией.
    Vec3 direction(int cx, int cz) const;
    bool empty() const { return dist_.empty(); }

private:
    static constexpr uint16_t kUnreach = 0xFFFFu;

    int minX_ = 0, minZ_ = 0, w_ = 0, h_ = 0;
    std::vector<uint16_t> dist_;
    std::vector<int32_t> parent_;  // индекс родителя; -1 = цель; -2 = недостижимо

    int idx(int cx, int cz) const;
};
