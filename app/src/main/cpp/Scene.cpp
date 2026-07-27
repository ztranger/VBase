#include "Scene.h"

#include <cmath>

#include "Assets.h"
#include "Log.h"
#include "Renderer.h"

void Scene::build(Renderer& renderer, AAssetManager* assets) {
    // --- Текстуры ---
    TextureHandle checker = renderer.createTexture(makeCheckerboard(256, 8));
    TextureHandle crateTex = 0;
    {
        TextureData img;
        if (loadImageAsset(assets, "textures/crate.png", img)) {
            crateTex = renderer.createTexture(img);
        }
    }

    // --- Материалы (окружение) ---
    MaterialHandle floorMat  = renderer.createMaterial({ShaderType::Lit,   {1.0f, 1.0f, 1.0f}, checker});
    MaterialHandle crateMat  = renderer.createMaterial({ShaderType::Lit,   {1.0f, 1.0f, 1.0f}, crateTex});
    MaterialHandle redMat    = renderer.createMaterial({ShaderType::Phong, {0.90f, 0.35f, 0.30f}, 0});
    MaterialHandle flatGreen = renderer.createMaterial({ShaderType::Unlit, {0.30f, 0.85f, 0.45f}, 0});
    MaterialHandle blueMat   = renderer.createMaterial({ShaderType::Phong, {0.35f, 0.55f, 0.95f}, 0});
    MaterialHandle goldMat   = renderer.createMaterial({ShaderType::Phong, {0.95f, 0.80f, 0.25f}, 0});

    // --- Меши окружения ---
    MeshHandle plane = renderer.createMesh(makePlane(24.0f, 12.0f));
    MeshHandle cube = renderer.createMesh(makeCube(1.0f));
    MeshHandle sphere = renderer.createMesh(makeSphere(0.6f));

    objects_.clear();

    {  // пол (побольше — есть где ходить)
        GameObject ground;
        ground.mesh = plane;
        ground.material = floorMat;
        objects_.push_back(ground);
    }

    const MaterialHandle cubeMats[] = {crateMat, redMat, flatGreen};
    for (int i = 0; i < 3; ++i) {
        GameObject c;
        c.mesh = cube;
        c.material = cubeMats[i];
        c.transform.position = {-4.0f + (float)i * 4.0f, 0.5f, -4.0f};
        c.spin = 0.6f + 0.3f * (float)i;
        objects_.push_back(c);
    }
    const MaterialHandle sphereMats[] = {blueMat, goldMat, redMat};
    for (int i = 0; i < 3; ++i) {
        GameObject s;
        s.mesh = sphere;
        s.material = sphereMats[i];
        s.transform.position = {-4.0f + (float)i * 4.0f, 0.6f, 4.0f};
        objects_.push_back(s);
    }

    // Кольцо инстансных кубов (демонстрация инстансинга) — как декор по краю.
    MaterialHandle ringMat = renderer.createMaterial({ShaderType::Lit, {0.55f, 0.65f, 0.85f}, checker});
    const int ringCount = 48;
    const float pi = 3.14159265358979323846f;
    for (int i = 0; i < ringCount; ++i) {
        float a = 2.0f * pi * (float)i / (float)ringCount;
        GameObject c;
        c.mesh = cube;
        c.material = ringMat;
        c.transform.position = {std::cos(a) * 10.0f, 0.4f, std::sin(a) * 10.0f};
        c.transform.scale = {0.4f, 0.4f, 0.4f};
        c.spin = 1.2f;
        objects_.push_back(c);
    }

    // --- Управляемый персонаж: лиса (glTF + скиннинг) ---
    if (loadGltfModel(assets, "models/Fox.glb", foxModel_)) {
        player_.model = &foxModel_;
        player_.mesh = renderer.createSkinnedMesh(foxModel_);
        if (foxModel_.hasTexture) {
            player_.tex = renderer.createTexture(foxModel_.baseColor);
        }
        player_.position = {0.0f, 0.0f, 0.0f};
    } else {
        LOGW("Не удалось загрузить Fox.glb");
    }
}

void Scene::setUiScale(float s) {
    uiScale_ = s;
    joystick_.radius = 120.0f * s;  // джойстик крупнее на плотных экранах
}

void Scene::update(float dt) {
    for (GameObject& obj : objects_) {
        obj.transform.rotation.y += obj.spin * dt;
    }

    // Джойстик -> направление в мире ОТНОСИТЕЛЬНО камеры (вверх на стике =
    // "от камеры вперёд"). Персонаж доворачивается к направлению движения.
    Vec3 moveDir{0.0f, 0.0f, 0.0f};
    float mag = joystick_.mag;
    if (mag > 0.05f) {
        float cy = camera_.yaw;
        Vec3 fwd{std::sin(cy), 0.0f, std::cos(cy)};
        Vec3 right{-std::cos(cy), 0.0f, std::sin(cy)};  // экранный right = cross(fwd, up)
        moveDir = normalize(fwd * joystick_.y + right * joystick_.x);
    }
    // Назад (стик вниз) — вдвое медленнее и БЕЗ доворота: лиса пятится, а не
    // крутится (при довороте назад камера-follow даёт вращение на месте).
    bool faceMove = joystick_.y >= 0.0f;
    if (joystick_.y < 0.0f) {
        mag *= 0.5f;
    }
    player_.update(dt, moveDir, mag, faceMove);

    // Камера следует за обобщённой целью (позиция + facing персонажа).
    camera_.follow(player_.position, player_.facingYaw, dt);
}

void Scene::onPointer(float x, float y, bool pressed) {
    joystick_.onPointer(x, y, pressed);
}

RenderFrame Scene::buildFrame(float aspect) const {
    RenderFrame frame;
    frame.view = camera_.view();
    frame.proj = camera_.proj(aspect);
    frame.cameraPos = camera_.eye();
    frame.lightDir = normalize(Vec3{0.4f, 1.0f, 0.6f});

    for (const GameObject& obj : objects_) {
        frame.items.push_back({obj.mesh, obj.material, obj.transform.matrix()});
    }
    if (player_.mesh != 0) {
        frame.skinned.push_back(player_.buildItem());
    }
    return frame;
}
