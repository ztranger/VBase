#include "game/CharacterRoster.h"

#include <cstdint>
#include <cstdlib>
#include <sstream>

#include "engine/assets/AssetSource.h"
#include "engine/core/Log.h"

namespace {

using Tokens = std::vector<std::string>;

Tokens tokenize(const std::string& s) {
    Tokens t;
    std::istringstream in(s);
    std::string w;
    while (in >> w) t.push_back(w);
    return t;
}

float toF(const std::string& s) { return std::strtof(s.c_str(), nullptr); }

// Разбор списка "a,b,c" по запятым в вектор непустых элементов.
void splitCommas(const std::string& list, std::vector<std::string>& out) {
    size_t s = 0;
    while (s < list.size()) {
        size_t e = list.find(',', s);
        if (e == std::string::npos) e = list.size();
        std::string item = list.substr(s, e - s);
        if (!item.empty()) out.push_back(item);
        s = e + 1;
    }
}

}  // namespace

bool loadCharacterRoster(AssetSource& assets, const char* path, std::vector<CharacterDesc>& out) {
    std::vector<uint8_t> bytes;
    if (!assets.read(path, bytes)) {
        LOGW("roster: файл не найден: %s", path);
        return false;
    }
    out.clear();

    std::string text(bytes.begin(), bytes.end());
    std::istringstream in(text);
    std::string raw;
    int line = 0;
    while (std::getline(in, raw)) {
        ++line;
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();  // CRLF-файлы
        size_t hash = raw.find('#');  // комментарий до конца строки
        if (hash != std::string::npos) raw.erase(hash);
        Tokens t = tokenize(raw);
        if (t.empty()) continue;

        if (t[0] != "character") {
            LOGE("roster: строка %d: ожидалось 'character', а не '%s'", line, t[0].c_str());
            continue;
        }
        if (t.size() < 2) { LOGE("roster: строка %d: нет id", line); continue; }

        CharacterDesc c;
        c.id = t[1];
        size_t i = 2;
        while (i < t.size()) {
            std::string k = t[i++];
            if (k == "name") { if (i < t.size()) c.name = t[i++]; }
            else if (k == "model") { if (i < t.size()) c.model = t[i++]; }
            else if (k == "scale") { if (i < t.size()) c.scale = toF(t[i++]); }
            else if (k == "yaw") { if (i < t.size()) c.yawOffset = toF(t[i++]); }
            else if (k == "hide") { if (i < t.size()) splitCommas(t[i++], c.hide); }
            else if (k == "attack") { if (i < t.size()) c.attackClip = t[i++]; }
            else if (k == "hp") { if (i < t.size()) c.hp = toF(t[i++]); }
            else if (k == "damage") { if (i < t.size()) c.damage = toF(t[i++]); }
            else if (k == "speed") { if (i < t.size()) c.speed = toF(t[i++]); }
            else if (k == "atkint") { if (i < t.size()) c.attackInterval = toF(t[i++]); }
            else if (k == "target") {
                if (i < t.size()) {
                    const std::string& v = t[i++];
                    if (v == "core") c.goal = CharacterDesc::MobGoal::Core;
                    else if (v == "building") c.goal = CharacterDesc::MobGoal::Building;
                    else LOGW("roster: строка %d: неизвестный target '%s' (core|building)",
                              line, v.c_str());
                }
            }
            else LOGW("roster: строка %d: неизвестный ключ '%s'", line, k.c_str());
        }
        if (c.model.empty()) {
            LOGE("roster: строка %d: у '%s' не задан model", line, c.id.c_str());
            continue;
        }
        if (c.name.empty()) c.name = c.id;
        out.push_back(std::move(c));
    }

    LOGI("Ростер персонажей: загружено %d из %s", (int)out.size(), path);
    return !out.empty();
}
