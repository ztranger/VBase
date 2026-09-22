#pragma once

#include <string>

#include "game/BuildingConfig.h"
#include "game/SceneDesc.h"

struct AssetSource;

// Разобрать текстовый файл сцены (через AssetSource, поэтому платформонезависимо).
// Формат — построчный, '#' начинает комментарий; см. assets/scenes/*.scene.
// false — файл не найден или синтаксическая ошибка (детали в лог).
bool loadSceneDesc(AssetSource& assets, const char* path, SceneDesc& out);

// Разбор сцены из строки (без I/O) — ядро loadSceneDesc; отдельно нужен редактору и round-trip
// самотесту (парс -> сериализация -> репарс без обращения к диску).
bool parseSceneDesc(const std::string& text, SceneDesc& out);

// Обратная операция к parseSceneDesc: сериализовать описание сцены в тот же текстовый формат
// (для редактора сцен — Save). Пишет ТОЛЬКО директивы, которые читает парсер (scene-authored):
// параметры зданий из config (rate/cap/wave*) НЕ эмитятся — их источник config/buildings.cfg,
// поэтому кормить сюда СЫРОЙ desc (до applyBuildingConfig). serialize∘parse идемпотентна.
std::string serializeSceneDesc(const SceneDesc& desc);

// Загрузить конфиг типов зданий (config/buildings.cfg): имена, описания, параметры.
// false — файл не найден (детали в лог); сцена без конфига работает на дефолтах.
bool loadBuildingConfig(AssetSource& assets, const char* path, BuildingConfig& out);

// Применить параметры из конфига к зданиям сцены (rate/cap по типу — из конфига).
// Единый источник настроек: сцена только размещает, конфиг задаёт параметры.
void applyBuildingConfig(SceneDesc& desc, const BuildingConfig& cfg);

// P2-12: пост-парс санитизация недоверенного описания сцены — клампит числовые поля в безопасные
// диапазоны и вычищает NaN/inf (деление на ноль, огромные аллокации, NaN в физике). Всегда делает
// desc пригодным (не отвергает). Звать после applyBuildingConfig на клиенте И в GameWorld на сервере.
void validateSceneDesc(SceneDesc& desc);
