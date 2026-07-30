#pragma once

#include "BuildingConfig.h"
#include "SceneDesc.h"

struct AssetSource;

// Разобрать текстовый файл сцены (через AssetSource, поэтому платформонезависимо).
// Формат — построчный, '#' начинает комментарий; см. assets/scenes/*.scene.
// false — файл не найден или синтаксическая ошибка (детали в лог).
bool loadSceneDesc(AssetSource& assets, const char* path, SceneDesc& out);

// Загрузить конфиг типов зданий (config/buildings.cfg): имена, описания, параметры.
// false — файл не найден (детали в лог); сцена без конфига работает на дефолтах.
bool loadBuildingConfig(AssetSource& assets, const char* path, BuildingConfig& out);

// Применить параметры из конфига к зданиям сцены (rate/cap по типу — из конфига).
// Единый источник настроек: сцена только размещает, конфиг задаёт параметры.
void applyBuildingConfig(SceneDesc& desc, const BuildingConfig& cfg);
