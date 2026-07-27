#pragma once

#include <cstdint>
#include <vector>

// Абстрактный источник ассетов: прочитать файл по логическому пути целиком.
// Реализации: Android (AAssetManager) и десктоп (файловая система). Загрузчики
// (текстуры/glTF/OBJ) работают через него и потому платформонезависимы.
struct AssetSource {
    virtual ~AssetSource() = default;

    // Прочитать весь файл в out. false — не найден/ошибка.
    virtual bool read(const char* path, std::vector<uint8_t>& out) = 0;
};
