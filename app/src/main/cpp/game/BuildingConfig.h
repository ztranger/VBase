#pragma once

#include <cstdint>
#include <string>

#include "engine/core/MathUtil.h"  // Vec3 (цвет визуала)
#include "engine/core/Texture.h"   // ShaderType (визуал)
#include "engine/net/Net.h"        // EntityType

// Форма процедурного меша визуала (клиент). None -> тип не рисуется generic-путём (напр. Hero).
enum class MeshShape : uint8_t { None, Cube, Sphere };

// Описания и настройки типов зданий/сущностей — из конфиг-файла (config/buildings.cfg).
// Одно место для геймплейных параметров, текстов панели И визуалов; сервер читает геймплей,
// клиент — ещё и визуал (shape/material/yOffset/pickRadius; сервер их просто игнорирует).
struct BuildingInfo {
    bool defined = false;
    std::string name;     // отображаемое имя (панель)
    std::string desc;     // описание (панель)
    float rate = 0.0f;    // generator: ресурс/сек; spawner: интервал; tower: интервал выстрела;
                          // enemy: интервал удара по ядру
    float cap = 0.0f;     // storage: ёмкость; spawner: максимум врагов
    float hp = 0.0f;      // core/enemy: здоровье
    float damage = 0.0f;  // tower: урон выстрела; enemy: урон по ядру
    float range = 0.0f;   // tower: радиус поражения
    float cost = 0.0f;    // стоимость постройки героем (0 = нельзя ставить в рантайме)

    // --- Бесконечные волны спавнера (0 = легаси: спавн до cap/max, затем стоп) ---
    int   waveSize = 0;     // spawner: врагов в волне; >0 включает бесконечные волны
    float wavePause = 0.0f; // spawner: пауза между волнами, сек
    int   waveGrow = 0;     // spawner: прирост размера волны за каждую волну (аддитивно)

    // --- Визуал (только клиент; см. Scene::build -> таблица EntityVisual) ---
    // Статичная OBJ-модель (напр. KayKit dungeon): путь + текстура-атлас + запекаемые
    // масштаб/поворот. Если задана и загрузилась — используется вместо процедурной shape.
    std::string model;          // путь к .obj (пусто = процедурная форma по shape)
    std::string modelTex;       // путь к текстуре-атласу (albedo)
    float modelScale = 1.0f;    // запекается в вершины (generic-рендер без масштаба)
    float modelYawDeg = 0.0f;   // поворот вокруг Y, градусы (запекается)
    MeshShape shape = MeshShape::None;
    float shapeSize = 1.0f;    // cube: сторона; sphere: радиус
    int shapeStacks = 16;      // sphere
    int shapeSlices = 24;      // sphere
    ShaderType shader = ShaderType::Lit;
    Vec3 color{1.0f, 1.0f, 1.0f};
    float yOffset = 0.0f;      // подъём центра над позицией (рендер И пикинг)
    float pickRadius = 0.0f;   // радиус сферы пикинга
};

// Таблица по EntityType (Generator/Storage/Spawner/Enemy/Tower/Core/Hero).
struct BuildingConfig {
    BuildingInfo byType[8];
    const BuildingInfo& get(EntityType t) const { return byType[(int)t]; }
};
