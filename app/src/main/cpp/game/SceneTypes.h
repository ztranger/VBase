#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "engine/assets/Mesh.h"     // MeshHandle
#include "engine/assets/Model.h"    // SkinnedHandle, SkinnedModel
#include "engine/core/Input.h"      // InputCommand
#include "engine/core/MathUtil.h"   // Vec3, Mat4
#include "engine/core/Texture.h"    // MaterialHandle, TextureHandle
#include "game/Character.h"         // Character

// P2-18 (шаг 1): plain-структуры данных клиентской сцены, вынесенные из Scene.h БЕЗ изменения
// владения/логики. Группирует типы четырёх будущих подсистем (сессия/мир/презентация/ресурсы),
// чтобы декомпозиция Scene шла поверх общего словаря типов. Чистые данные, без методов сцены.

// --- Ресурсы окружения ---

// Позиция/поворот/масштаб статичного объекта окружения.
struct Transform {
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 rotation{0.0f, 0.0f, 0.0f};
    Vec3 scale{1.0f, 1.0f, 1.0f};

    Mat4 matrix() const {
        return Mat4::translation(position)
             * Mat4::rotationY(rotation.y)
             * Mat4::rotationX(rotation.x)
             * Mat4::rotationZ(rotation.z)
             * Mat4::scale(scale);
    }
};

// Статичный объект окружения (пол, кубы, сферы, кольцо).
struct GameObject {
    Transform transform;
    MeshHandle mesh = 0;
    MaterialHandle material = 0;
    float spin = 0.0f;
    float prevRotY = 0.0f;  // для интерполяции вращения между тиками
    // Для редактора сцен: локальный AABB меша (пикинг/подсветка) + индекс исходного ObjectSpec
    // в sceneDesc_.objects (-1 = не редактируется поштучно, напр. копии кольца).
    Vec3 aabbMin{0.0f, 0.0f, 0.0f};
    Vec3 aabbMax{0.0f, 0.0f, 0.0f};
    int specIndex = -1;
    TextureHandle albedo = 0;  // альбедо модели (чтобы редактор пересобрал материал под новый шейдер/цвет)
};

// Реестр выбираемых персонажей/мобов (грузится один раз в build; индекс = charType в снапшоте).
struct PlayerModel {
    std::string id, name;
    SkinnedModel model;          // данные скелета/анимаций
    SkinnedHandle mesh = 0;      // GPU-меш (клиентский ресурс)
    TextureHandle tex = 0;
    float scale = 1.0f, yawOffset = 0.0f;
    int idleClip = 0, walkClip = 1, runClip = 2, attackClip = -1, deathClip = -1;
    float attackClipDur = 0.0f;  // длительность клипа атаки, сек (масштаб под kAttackDuration)
    float deathClipDur = 0.0f;   // длительность клипа смерти, сек (для «трупа» моба)
    float hp = 0.0f;             // статы героя (для HUD/предсказания): 0 = дефолт
    float speed = 0.0f;          // скорость бега героя (client player_.maxSpeed для предсказания)
    float damage = 0.0f;         // урон авто-атаки (для брифинга/выбора; клиент не считает бой)
    float range = 0.0f;          // дальность авто-атаки (0 = не атакует)
    bool  ranged = false;        // true -> дальний (стреляет), false -> ближний
};

// Клиентский визуал/пикинг по типу сущности — ОДНА таблица вместо разбросанных switch.
struct EntityVisual {
    MeshHandle mesh = 0;         // 0 = не рисуется generic-путём
    MaterialHandle material = 0;
    float yOffset = 0.0f;        // подъём центра над позицией (рендер И пикинг)
    float pickRadius = 0.0f;     // радиус сферы пикинга
    bool pickable = false;       // выбирается кликом (Hero — нет)
    bool building = false;       // занимает клетку сетки (для стройки)
};

// Запись манифеста доступных сцен (config/scenes.cfg): путь + отображаемое имя.
struct SceneEntry {
    std::string path, name;
};

// --- Мир / сессия ---

// Снапшот состояния во времени (для буфера интерполяции чужих игроков).
struct TimedState {
    double t = 0.0;  // время приёма (по часам симуляции), сек
    Vec3 pos{0.0f, 0.0f, 0.0f};
    float yaw = 0.0f;
    float anim = 0.0f;
    float attack = 0.0f;  // остаток времени атаки (для рендера каста чужих)
};

// Чужая сущность (не свой герой): любой тип из снапшота — герой, генератор, хранилище, враг, …
struct RemoteEntity {
    uint32_t id = 0;
    uint8_t type = 0;   // EntityType
    uint8_t team = 0;
    uint8_t charType = 0;  // индекс персонажа в ростере (какой моделью рисовать чужого героя)
    Character ch;       // трансформ для рендера/интерполяции
    std::vector<TimedState> buffer;
    float aux = 0.0f;   // ресурс в хранилище и т.п. (последнее значение, без интерполяции)
    float hp = 0.0f;    // здоровье (ядро/враг) из снапшота
};

// Отправленная, но ещё не подтверждённая сервером команда (для реплея реконсиляции).
struct PendingInput {
    InputCommand cmd;
    float dt = 0.0f;
};

// «Труп» убитого моба: локальная косметика — проигрывает клип смерти на месте гибели.
struct DyingMob {
    int charType = 0;
    Vec3 pos{0.0f, 0.0f, 0.0f};
    float yaw = 0.0f;
    float t = 0.0f;    // время с начала смерти
    float dur = 1.0f;  // длительность клипа смерти
};

// --- Презентация боя (клиентская косметика; Scene ПРОИЗВОДИТ, GameUi рисует) ---

struct CombatMarker { Vec3 pos; float hpFrac; Vec3 color; };            // полоска HP над сущностью
struct DamageNumber { Vec3 pos; float amount; float age; Vec3 color; }; // всплывающее число урона
struct ImpactSpark { Vec3 pos; float age; float maxAge; uint32_t seed; };  // вспышка-искры в точке хита
struct BuildPoof { Vec3 pos; float age; };  // «пуф» при постройке (расходящееся кольцо)
