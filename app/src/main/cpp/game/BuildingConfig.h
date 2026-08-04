#pragma once

#include <string>

#include "engine/net/Net.h"  // EntityType

// Описания и настройки типов зданий/сущностей — из конфиг-файла (config/buildings.cfg).
// Одно место для геймплейных параметров и текстов панели; и сервер, и клиент читают его.
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
};

// Таблица по EntityType (Generator/Storage/Spawner/Enemy/Tower/Core/Hero).
struct BuildingConfig {
    BuildingInfo byType[8];
    const BuildingInfo& get(EntityType t) const { return byType[(int)t]; }
};
