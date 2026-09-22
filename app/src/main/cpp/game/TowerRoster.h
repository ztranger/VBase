#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "engine/core/MathUtil.h"   // Vec3 (цвет визуала)
#include "game/GameTypes.h"         // EntityType (tower/trap → тег сущности)

class AssetSource;

// Один вид защиты (башня-стрелок или напольная ловушка) — данные из config/towers.cfg.
// Индекс записи = «вид» (kind), едет клиенту в младшем ниббле EntityState.charType у Tower/Trap
// (тир — в старшем ниббле). ВАЖНО: порядок записей = сетевой контракт индексов kind; при
// перестановке/удалении бампать kProtocolVersion (Net.h). Сервер читает бой, клиент — визуал/цену.
struct TowerDesc {
    std::string id;
    std::string name;                      // короткое имя для палитры (один токен)
    EntityType entity = EntityType::Tower; // tower (стрелок) | trap (пол): решает поведение/футпринт

    // Базовые статы (тир 1). Апгрейд множит их (см. tier*Mul ниже).
    float cost = 0.0f;     // стоимость постройки
    float hp = 0.0f;       // прочность (башни ломаемы мобами; ловушки — нет)
    float rate = 0.0f;     // tower: интервал выстрела, сек; trap: интервал тика AoE, сек
    float range = 0.0f;    // tower: радиус поражения; trap: радиус тика
    float damage = 0.0f;   // урон за выстрел/тик

    // Эффект попадания (tower — при попадании снаряда; trap — по всем в радиусе на тике).
    enum class Effect : uint8_t { None = 0, Slow, Splash, Burn } effect = Effect::None;
    float slowFactor = 0.5f;   // Slow: скорость цели *= slowFactor на slowDur сек
    float effectDur = 0.0f;    // Slow/Burn: длительность, сек
    float splashRadius = 0.0f; // Splash: радиус доп. урона по площади вокруг попадания
    float burnDps = 0.0f;      // Burn: урон/сек, пока горит (effectDur сек)

    // Апгрейд: тир 1..maxTier. Статы на тире T = база * mul^(T-1).
    int   maxTier = 3;
    float tierDamageMul = 1.6f;   // урон растёт
    float tierRangeMul = 1.12f;   // радиус растёт
    float tierRateMul = 0.85f;    // интервал падает (стреляет чаще)
    float upgradeCostMul = 0.8f;  // цена апгрейда T→T+1 = cost * upgradeCostMul * T

    Vec3 color{1.0f, 1.0f, 1.0f};  // тинт визуала (клиент): цвет башни/ловушки и болта
};

// Загрузка ростера видов защиты из текстового конфига (через AssetSource). Формат — по строке:
//   tower <id> [class tower|trap] [name <w>] cost <c> hp <h> rate <r> range <rg> damage <d>
//     [effect none|slow|splash|burn] [slow <f>] [dur <s>] [splash <rad>] [burn <dps>]
//     [maxtier <n>] [dmgmul <m>] [rangemul <m>] [ratemul <m>] [upcost <m>] [color <r> <g> <b>]
// false = файл не прочитан или пуст (тогда рантайм-постройка башен недоступна — только сцена).
bool loadTowerRoster(AssetSource& assets, const char* path, std::vector<TowerDesc>& out);

// Статы вида на заданном тире (1-based): множит базу на tier*Mul^(tier-1). Клампит tier в [1,maxTier].
struct TowerTierStats { float rate, range, damage; };
TowerTierStats towerStatsForTier(const TowerDesc& d, int tier);

// Цена апгрейда с текущего тира на следующий (tier → tier+1). 0, если апгрейд невозможен.
float towerUpgradeCost(const TowerDesc& d, int tier);
