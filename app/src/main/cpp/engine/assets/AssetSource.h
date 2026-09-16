#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Максимальный размер одного ассета (P2-13): защита от гигантских файлов (OOM/ANR на Android).
// 64 МБ — с большим запасом на модели/текстуры проекта; недоверенный файл сверх — отвергаем.
inline constexpr size_t kMaxAssetBytes = 64u * 1024u * 1024u;

// Безопасен ли логический путь ассета (P2-14): не выходит за корень assets. Отвергаем абсолютные
// пути, диск/схему (двоеточие) и компоненты «..». Пути в проекте — относительные с прямыми
// слэшами ("scenes/x.scene"); недоверенные (из сцен/конфигов) обязаны пройти эту проверку.
inline bool assetPathIsSafe(const char* path) {
    if (path == nullptr || path[0] == '\0') return false;
    if (path[0] == '/' || path[0] == '\\') return false;  // абсолютный путь
    for (const char* p = path; *p != '\0'; ++p) {
        if (*p == ':') return false;  // диск/схема (C:\, file:)
        // Компонент ".." (в начале строки или сразу после разделителя) = выход вверх.
        if (p[0] == '.' && p[1] == '.' && (p[2] == '\0' || p[2] == '/' || p[2] == '\\') &&
            (p == path || p[-1] == '/' || p[-1] == '\\')) {
            return false;
        }
    }
    return true;
}

// Абстрактный источник ассетов: прочитать файл по логическому пути целиком.
// Реализации: Android (AAssetManager) и десктоп (файловая система). Загрузчики
// (текстуры/glTF/OBJ) работают через него и потому платформонезависимы.
struct AssetSource {
    virtual ~AssetSource() = default;

    // Прочитать весь файл в out. false — не найден/ошибка/путь небезопасен/файл слишком большой.
    virtual bool read(const char* path, std::vector<uint8_t>& out) = 0;
};
