#pragma once

#include <string>
#include <vector>

#include "MathUtil.h"
#include "Texture.h"  // ShaderType

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

struct PlayerSpec {
    bool present = false;
    std::string model;   // путь к glTF (скиннинг)
    Vec3 pos{0.0f, 0.0f, 0.0f};
    float scale = 0.03f;
    float yawOffset = 0.0f;  // подгонка "морда по движению"
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
    PlayerSpec player;
    CameraSpec camera;
    Vec3 lightDir{0.4f, 1.0f, 0.6f};  // направление НА источник света
};
