#pragma once

#include <string>
#include <vector>

class AssetSource;

// Один выбираемый персонаж (данные экрана выбора). Грузится из config/characters.cfg.
// Чистые данные, без рендера — клиент по ним строит скиннинг-модель в Scene::build.
struct CharacterDesc {
    std::string id;                 // стабильный идентификатор (логи/отладка)
    std::string name;               // отображаемое имя (экран выбора)
    std::string model;              // путь к .glb
    float scale = 1.0f;
    float yawOffset = 0.0f;
    std::vector<std::string> hide;  // подстроки имён узлов-реквизита, которые прятать
    std::string attackClip;         // имя клипа атаки; пусто -> keyword-поиск в Scene
};

// Загрузка ростера из текстового конфига (через AssetSource). Формат — по строке:
//   character <id> [name <name>] model <path> [scale <s>] [yaw <o>] [hide <a,b,c>] [attack <clip>]
// ВАЖНО: порядок записей = сетевой контракт индексов charType (см. Net.h EntityState). При
// перестановке/удалении записей бампать kProtocolVersion. false = файл не прочитан или пуст.
bool loadCharacterRoster(AssetSource& assets, const char* path, std::vector<CharacterDesc>& out);
