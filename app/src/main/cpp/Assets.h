#pragma once

#include <cstddef>

#include "AssetSource.h"
#include "Mesh.h"
#include "Texture.h"

// Загрузчики ассетов — работают через AssetSource, поэтому платформонезависимы
// (Android/десктоп). Возвращают CPU-данные, которые заливает в GPU рендер.

// Процедурная шахматная текстура (без файла): size x size, cells клеток.
TextureData makeCheckerboard(uint32_t size, uint32_t cells);

// Декодировать изображение (PNG/JPG/...) из assets через stb_image.
bool loadImageAsset(AssetSource& src, const char* path, TextureData& out);

// Декодировать изображение из байтов в памяти (например, встроенное в .glb).
bool decodeImageBuffer(const void* data, size_t size, TextureData& out);

// Разобрать Wavefront OBJ из assets (v/vt/vn + полигоны, нормали по граням).
bool loadObjAsset(AssetSource& src, const char* path, MeshData& out);
