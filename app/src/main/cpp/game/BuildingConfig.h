#pragma once

#include <string>

#include "engine/net/Net.h"  // EntityType

// Описания и настройки типов зданий/сущностей — из конфиг-файла (config/buildings.cfg).
// Одно место для геймплейных параметров и текстов панели; и сервер, и клиент читают его.
struct BuildingInfo {
    bool defined = false;
    std::string name;   // отображаемое имя (панель)
    std::string desc;   // описание (панель)
    float rate = 0.0f;  // generator: ресурс/сек; spawner: интервал спавна, сек
    float cap = 0.0f;   // storage: ёмкость; spawner: максимум врагов
};

// Таблица по EntityType (Generator/Storage/Spawner/Enemy/Tower/Core/Hero).
struct BuildingConfig {
    BuildingInfo byType[8];
    const BuildingInfo& get(EntityType t) const { return byType[(int)t]; }
};
