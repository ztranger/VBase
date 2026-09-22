#include "game/TowerRoster.h"

#include <cmath>
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
int toI(const std::string& s) { return (int)std::strtol(s.c_str(), nullptr, 10); }

}  // namespace

bool loadTowerRoster(AssetSource& assets, const char* path, std::vector<TowerDesc>& out) {
    std::vector<uint8_t> bytes;
    if (!assets.read(path, bytes)) {
        LOGW("towers: файл не найден: %s", path);
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

        if (t[0] != "tower") {
            LOGE("towers: строка %d: ожидалось 'tower', а не '%s'", line, t[0].c_str());
            continue;
        }
        if (t.size() < 2) { LOGE("towers: строка %d: нет id", line); continue; }

        TowerDesc d;
        d.id = t[1];
        size_t i = 2;
        while (i < t.size()) {
            std::string k = t[i++];
            if (k == "class") {
                if (i < t.size()) {
                    const std::string& v = t[i++];
                    if (v == "tower") d.entity = EntityType::Tower;
                    else if (v == "trap") d.entity = EntityType::Trap;
                    else LOGW("towers: строка %d: неизвестный class '%s' (tower|trap)", line, v.c_str());
                }
            } else if (k == "name") { if (i < t.size()) d.name = t[i++]; }
            else if (k == "cost") { if (i < t.size()) d.cost = toF(t[i++]); }
            else if (k == "hp") { if (i < t.size()) d.hp = toF(t[i++]); }
            else if (k == "rate") { if (i < t.size()) d.rate = toF(t[i++]); }
            else if (k == "range") { if (i < t.size()) d.range = toF(t[i++]); }
            else if (k == "damage") { if (i < t.size()) d.damage = toF(t[i++]); }
            else if (k == "effect") {
                if (i < t.size()) {
                    const std::string& v = t[i++];
                    if (v == "none") d.effect = TowerDesc::Effect::None;
                    else if (v == "slow") d.effect = TowerDesc::Effect::Slow;
                    else if (v == "splash") d.effect = TowerDesc::Effect::Splash;
                    else if (v == "burn") d.effect = TowerDesc::Effect::Burn;
                    else LOGW("towers: строка %d: неизвестный effect '%s'", line, v.c_str());
                }
            }
            else if (k == "slow") { if (i < t.size()) d.slowFactor = toF(t[i++]); }
            else if (k == "dur") { if (i < t.size()) d.effectDur = toF(t[i++]); }
            else if (k == "splash") { if (i < t.size()) d.splashRadius = toF(t[i++]); }
            else if (k == "burn") { if (i < t.size()) d.burnDps = toF(t[i++]); }
            else if (k == "maxtier") { if (i < t.size()) d.maxTier = toI(t[i++]); }
            else if (k == "dmgmul") { if (i < t.size()) d.tierDamageMul = toF(t[i++]); }
            else if (k == "rangemul") { if (i < t.size()) d.tierRangeMul = toF(t[i++]); }
            else if (k == "ratemul") { if (i < t.size()) d.tierRateMul = toF(t[i++]); }
            else if (k == "upcost") { if (i < t.size()) d.upgradeCostMul = toF(t[i++]); }
            else if (k == "color") {
                if (i + 2 < t.size()) {
                    d.color = {toF(t[i]), toF(t[i + 1]), toF(t[i + 2])};
                    i += 3;
                } else { LOGW("towers: строка %d: color требует 3 числа", line); i = t.size(); }
            }
            else LOGW("towers: строка %d: неизвестный ключ '%s'", line, k.c_str());
        }
        if (d.name.empty()) d.name = d.id;
        if (d.maxTier < 1) d.maxTier = 1;
        out.push_back(std::move(d));
    }

    LOGI("Ростер башен/ловушек: загружено %d из %s", (int)out.size(), path);
    return !out.empty();
}

TowerTierStats towerStatsForTier(const TowerDesc& d, int tier) {
    if (tier < 1) tier = 1;
    if (tier > d.maxTier) tier = d.maxTier;
    float m = (float)(tier - 1);
    TowerTierStats s;
    s.damage = d.damage * std::pow(d.tierDamageMul, m);
    s.range = d.range * std::pow(d.tierRangeMul, m);
    s.rate = d.rate * std::pow(d.tierRateMul, m);
    return s;
}

float towerUpgradeCost(const TowerDesc& d, int tier) {
    if (tier < 1 || tier >= d.maxTier) return 0.0f;  // уже макс. тир — апгрейд невозможен
    return d.cost * d.upgradeCostMul * (float)tier;
}
