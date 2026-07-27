#pragma once

#include <android/asset_manager.h>

#include "Mesh.h"
#include "Texture.h"

// Загрузчики ассетов — backend-агностичны: возвращают CPU-данные (MeshData/
// TextureData), которые потом заливает в GPU рендер. Про GL/Vulkan не знают.

// Процедурная шахматная текстура (не требует файла) — size x size, cells клеток.
TextureData makeCheckerboard(uint32_t size, uint32_t cells);

// Декодирование изображения (PNG/JPG/WebP) из assets/ через нативный AImageDecoder.
// Требует API 30+ (на более старых вернёт false — вызывающий откатывается на процедурную).
bool loadImageAsset(AAssetManager* mgr, const char* path, TextureData& out);

// То же, но из байтов в памяти (например, текстура, встроенная в .glb).
bool decodeImageBuffer(const void* data, size_t size, TextureData& out);

// Парсер Wavefront OBJ из assets/. Поддерживает v/vt/vn и полигональные грани
// (триангуляция веером). Нормали, если их нет в файле, считаются по граням.
bool loadObjAsset(AAssetManager* mgr, const char* path, MeshData& out);
