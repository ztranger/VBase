#pragma once

#include <cstdio>
#include <string>

#include "AssetSource.h"

// Источник ассетов из файловой системы (десктоп). Пути берутся относительно
// базовой директории (обычно каталог assets/).
class FileAssetSource : public AssetSource {
public:
    explicit FileAssetSource(std::string baseDir) : baseDir_(std::move(baseDir)) {}

    bool read(const char* path, std::vector<uint8_t>& out) override {
        std::string full = baseDir_;
        if (!full.empty() && full.back() != '/' && full.back() != '\\') full += '/';
        full += path;

        std::FILE* f = std::fopen(full.c_str(), "rb");
        if (f == nullptr) return false;
        std::fseek(f, 0, SEEK_END);
        long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (size < 0) { std::fclose(f); return false; }
        out.resize((size_t)size);
        size_t got = std::fread(out.data(), 1, (size_t)size, f);
        std::fclose(f);
        return got == (size_t)size;
    }

private:
    std::string baseDir_;
};
