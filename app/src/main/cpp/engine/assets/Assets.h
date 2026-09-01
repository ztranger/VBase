#pragma once

#include <cstddef>

#include "engine/assets/AssetSource.h"
#include "engine/assets/Mesh.h"
#include "engine/core/Texture.h"

// Загрузчики ассетов — работают через AssetSource, поэтому платформонезависимы
// (Android/десктоп). Возвращают CPU-данные, которые заливает в GPU рендер.

// Процедурная шахматная текстура (без файла): size x size, cells клеток.
TextureData makeCheckerboard(uint32_t size, uint32_t cells);

// Процедурная нормал-карта (tangent-space): бампы синусом, freq волн по стороне.
// RGB = закодированная нормаль ((n*0.5+0.5)*255), пригодна для uNormalMap.
TextureData makeBumpNormal(uint32_t size, uint32_t freq);

// Декодировать изображение (PNG/JPG/...) из assets через stb_image.
bool loadImageAsset(AssetSource& src, const char* path, TextureData& out);

// Декодировать изображение из байтов в памяти (например, встроенное в .glb).
bool decodeImageBuffer(const void* data, size_t size, TextureData& out);

// Разобрать Wavefront OBJ из assets (v/vt/vn + полигоны, нормали по граням).
bool loadObjAsset(AssetSource& src, const char* path, MeshData& out);
