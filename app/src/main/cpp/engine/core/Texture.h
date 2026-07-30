#pragma once

#include <cstdint>
#include <vector>

#include "engine/core/MathUtil.h"

// Пиксельные данные текстуры (RGBA8). Источник — процедурная генерация
// или декодирование изображения; рендер заливает это в GPU.
using TextureHandle = uint32_t;  // 0 — невалидный

struct TextureData {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;  // width*height*4 байт
};

// Какой шейдер использует материал. Backend-агностично: каждый рендер сам
// сопоставляет тип со своей программой. Значения = порядок сборки программ.
enum class ShaderType : uint32_t {
    Lit = 0,    // диффуз (Ламберт) + текстура
    Unlit = 1,  // плоский цвет/текстура без освещения
    Phong = 2,  // диффуз + зеркальный блик (нужна позиция камеры)
    Count
};

// Описание материала: шейдер + базовый цвет + одна альбедо-текстура.
using MaterialHandle = uint32_t;  // 0 — невалидный

struct MaterialDesc {
    ShaderType shader = ShaderType::Lit;
    Vec3 baseColor{1.0f, 1.0f, 1.0f};
    TextureHandle albedo = 0;  // 0 -> рендер подставит белую 1x1 (без ветвлений в шейдере)
};
