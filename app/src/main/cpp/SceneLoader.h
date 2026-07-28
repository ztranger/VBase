#pragma once

#include "SceneDesc.h"

struct AssetSource;

// Разобрать текстовый файл сцены (через AssetSource, поэтому платформонезависимо).
// Формат — построчный, '#' начинает комментарий; см. assets/scenes/*.scene.
// false — файл не найден или синтаксическая ошибка (детали в лог).
bool loadSceneDesc(AssetSource& assets, const char* path, SceneDesc& out);
