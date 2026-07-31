#pragma once

#include <string>
#include <vector>

#include "engine/core/MathUtil.h"
#include "engine/core/Texture.h"  // ShaderType

// Декларативное описание сцены (чистые данные, без рендера и платформы).
// Загружается из текстового файла (см. SceneLoader), затем Scene::build
// инстанцирует его в GPU-ресурсы и игровые объекты. Ссылки — по именам.

struct MeshSpec {
    std::string name;
    enum Kind { Plane, Cube, Sphere } kind = Cube;
    float a = 1.0f;     // plane: size; cube: size; sphere: radius
    float b = 1.0f;     // plane: uvTiles (иначе не используется)
    int stacks = 16;    // sphere
    int slices = 24;    // sphere
};

struct TextureSpec {
    std::string name;
    enum Kind { Checker, Image } kind = Checker;
    int size = 256;         // checker: сторона
    int cells = 8;          // checker: клеток
    std::string path;       // image: путь к файлу-картинке
};

struct MaterialSpec {
    std::string name;
    ShaderType shader = ShaderType::Lit;
    Vec3 color{1.0f, 1.0f, 1.0f};
    std::string tex;  // ссылка: имя текстуры ИЛИ путь к картинке; пусто -> без текстуры
};

struct ObjectSpec {
    std::string mesh;      // имя меша
    std::string material;  // имя материала
    Vec3 pos{0.0f, 0.0f, 0.0f};
    Vec3 rot{0.0f, 0.0f, 0.0f};
    float scale = 1.0f;    // равномерный масштаб
    float spin = 0.0f;     // авто-вращение вокруг Y, рад/с

    // Инстансинг по кольцу: если ring=true, объект размножается на count копий
    // по окружности радиуса radius на высоте y (pos игнорируется).
    bool ring = false;
    int ringCount = 0;
    float ringRadius = 0.0f;
    float ringY = 0.0f;
};

// Статичный коллайдер для физики (независим от визуальных мешей — коллизия может
// отличаться от отрисовки). Пока только бокс (AABB, без поворота).
struct ColliderSpec {
    enum Kind { Box } kind = Box;
    Vec3 center{0.0f, 0.0f, 0.0f};
    Vec3 half{0.5f, 0.5f, 0.5f};  // полуразмеры бокса
};

// Статичная сущность базы, заданная в сцене (сервер спавнит из неё сущность).
// Generator капает ресурс (rate/сек) в пул с потолком (сумма cap хранилищ);
// Spawner по таймеру (rate = интервал, сек) плодит врагов (cap = максимум);
// Core — ядро базы, цель для врагов (hp = здоровье);
// Tower — башня-защита (rate = интервал выстрела, damage = урон, range = радиус).
struct BuildingSpec {
    enum Kind { Generator, Storage, Spawner, Core, Tower } kind = Generator;
    Vec3 pos{0.0f, 0.0f, 0.0f};
    float rate = 0.0f;    // generator: ресурс/сек; spawner: интервал спавна; tower: интервал выстрела
    float cap = 0.0f;     // storage: ёмкость; spawner: макс. врагов
    float hp = 0.0f;      // core: здоровье
    float damage = 0.0f;  // tower: урон за выстрел
    float range = 0.0f;   // tower: радиус поражения
};

// Боевые параметры врага (враги не в сцене — их плодит спавнер; статы из конфига).
struct EnemySpec {
    float hp = 10.0f;             // здоровье
    float damage = 5.0f;          // урон по ядру за удар
    float attackInterval = 1.0f;  // секунд между ударами по ядру
};

struct PlayerSpec {
    bool present = false;
    std::string model;   // путь к glTF (скиннинг)
    Vec3 pos{0.0f, 0.0f, 0.0f};
    float scale = 0.03f;
    float yawOffset = 0.0f;  // подгонка "морда по движению"
    // Капсула контроллера (кинематическая физика). cylHalf — половина высоты цилиндра.
    float colliderRadius = 0.3f;
    float colliderCylHalf = 0.3f;
};

struct CameraSpec {
    float distance = 6.0f;
    float height = 3.0f;
    float lookHeight = 1.0f;
    float fovY = 1.0f;
    float nearZ = 0.1f;
    float farZ = 200.0f;
};

struct SceneDesc {
    std::vector<TextureSpec> textures;
    std::vector<MaterialSpec> materials;
    std::vector<MeshSpec> meshes;
    std::vector<ObjectSpec> objects;  // включая кольцевые (ring=true)
    std::vector<ColliderSpec> colliders;  // статичная геометрия для физики
    std::vector<BuildingSpec> buildings;  // здания базы (генераторы/хранилища/башни/ядро)
    EnemySpec enemy;                      // боевые статы врага (из конфига)
    PlayerSpec player;
    CameraSpec camera;
    Vec3 lightDir{0.4f, 1.0f, 0.6f};  // направление НА источник света
};
