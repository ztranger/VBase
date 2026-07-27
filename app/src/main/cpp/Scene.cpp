#include "Scene.h"

#include <cmath>

#include "Assets.h"
#include "Log.h"
#include "Renderer.h"

void Scene::build(Renderer& renderer, AAssetManager* assets) {
    // --- Текстуры ---
    // Процедурный чекер (файл не нужен) — им покроем пол.
    TextureHandle checker = renderer.createTexture(makeCheckerboard(256, 8));
    // Попытка загрузить текстуру из assets/textures/crate.png (если положишь файл);
    // при отсутствии — 0, материал будет просто цветным.
    TextureHandle crateTex = 0;
    {
        TextureData img;
        if (loadImageAsset(assets, "textures/crate.png", img)) {
            crateTex = renderer.createTexture(img);
        }
    }

    // --- Материалы (шейдер + цвет + опциональная текстура) ---
    // Разные шейдеры специально: пол/ящик — Lit, сферы — Phong (виден блик),
    // один куб — Unlit (плоский, «светящийся»).
    MaterialHandle floorMat  = renderer.createMaterial({ShaderType::Lit,   {1.0f, 1.0f, 1.0f}, checker});
    MaterialHandle crateMat  = renderer.createMaterial({ShaderType::Lit,   {1.0f, 1.0f, 1.0f}, crateTex});
    MaterialHandle redMat    = renderer.createMaterial({ShaderType::Phong, {0.90f, 0.35f, 0.30f}, 0});
    MaterialHandle flatGreen = renderer.createMaterial({ShaderType::Unlit, {0.30f, 0.85f, 0.45f}, 0});
    MaterialHandle blueMat   = renderer.createMaterial({ShaderType::Phong, {0.35f, 0.55f, 0.95f}, 0});
    MaterialHandle goldMat   = renderer.createMaterial({ShaderType::Phong, {0.95f, 0.80f, 0.25f}, 0});

    // --- Меши ---
    MeshHandle plane = renderer.createMesh(makePlane(12.0f, 6.0f));  // UV тайлятся 6x — виден чекер
    MeshHandle cube = renderer.createMesh(makeCube(1.0f));
    MeshHandle sphere = renderer.createMesh(makeSphere(0.6f));

    // Модель из файла — демонстрация загрузчика OBJ.
    MeshHandle pyramid = 0;
    {
        MeshData model;
        if (loadObjAsset(assets, "models/pyramid.obj", model)) {
            pyramid = renderer.createMesh(model);
        } else {
            LOGW("Не удалось загрузить pyramid.obj — модель не будет показана");
        }
    }

    objects_.clear();

    // Пол.
    {
        GameObject ground;
        ground.mesh = plane;
        ground.material = floorMat;
        objects_.push_back(ground);
    }

    // Ряд кубов (вращаются). Первый — текстурированный (если есть crate.png),
    // остальные — цветные. Материалы намеренно повторяются: сортировка в рендере
    // сгруппирует одинаковые и не будет лишний раз переключать состояние.
    const MaterialHandle cubeMats[] = {crateMat, redMat, flatGreen};
    for (int i = 0; i < 3; ++i) {
        GameObject c;
        c.mesh = cube;
        c.material = cubeMats[i];
        c.transform.position = {-3.0f + (float)i * 3.0f, 0.5f, -1.5f};
        c.spin = 0.6f + 0.3f * (float)i;
        objects_.push_back(c);
    }

    // Ряд сфер (статичны), цвета чередуются.
    const MaterialHandle sphereMats[] = {blueMat, goldMat, redMat};
    for (int i = 0; i < 3; ++i) {
        GameObject s;
        s.mesh = sphere;
        s.material = sphereMats[i];
        s.transform.position = {-3.0f + (float)i * 3.0f, 0.6f, 1.8f};
        objects_.push_back(s);
    }

    // Загруженная модель по центру.
    if (pyramid != 0) {
        GameObject p;
        p.mesh = pyramid;
        p.material = goldMat;
        p.transform.position = {0.0f, 0.0f, 0.0f};
        p.transform.scale = {1.5f, 1.5f, 1.5f};
        p.spin = 0.4f;
        objects_.push_back(p);
    }

    // Кольцо из множества одинаковых кубов: один меш + ОДИН материал ->
    // рендер нарисует их все ОДНИМ glDrawElementsInstanced. Демонстрация инстансинга.
    MaterialHandle ringMat = renderer.createMaterial({ShaderType::Lit, {0.55f, 0.65f, 0.85f}, checker});
    const int ringCount = 48;
    const float pi = 3.14159265358979323846f;
    for (int i = 0; i < ringCount; ++i) {
        float a = 2.0f * pi * (float)i / (float)ringCount;
        GameObject c;
        c.mesh = cube;
        c.material = ringMat;
        c.transform.position = {std::cos(a) * 6.0f, 0.4f, std::sin(a) * 6.0f};
        c.transform.scale = {0.4f, 0.4f, 0.4f};
        c.spin = 1.2f;
        objects_.push_back(c);
    }

    // Анимированная модель (glTF + скиннинг): Fox с анимациями Survey/Walk/Run.
    if (loadGltfModel(assets, "models/Fox.glb", foxModel_)) {
        foxMesh_ = renderer.createSkinnedMesh(foxModel_);
        if (foxModel_.hasTexture) {
            foxTex_ = renderer.createTexture(foxModel_.baseColor);
        }
        // Fox в нативных единицах крупный (~100), поэтому масштабируем.
        if (foxModel_.animations.size() > 1) foxAnim_ = 1;  // обычно Walk
    } else {
        LOGW("Не удалось загрузить Fox.glb");
    }
}

void Scene::update(float dt) {
    for (GameObject& obj : objects_) {
        obj.transform.rotation.y += obj.spin * dt;
    }
    foxTime_ += dt;  // продвигаем время анимации модели
}

const char* Scene::animationName(int i) const {
    if (i < 0 || i >= (int)foxModel_.animations.size()) return "";
    return foxModel_.animations[i].name.c_str();
}

void Scene::setAnimation(int i) {
    if (i >= 0 && i < (int)foxModel_.animations.size() && i != foxAnim_) {
        foxAnim_ = i;
        foxTime_ = 0.0f;  // с начала новой анимации
    }
}

void Scene::onPointer(float x, float y, bool pressed) {
    if (pressed) {
        if (dragging_) {
            camera_.yaw += (x - lastX_) * 0.005f;
            camera_.pitch += (y - lastY_) * 0.005f;
            const float limit = 1.4f;
            if (camera_.pitch > limit) camera_.pitch = limit;
            if (camera_.pitch < -limit) camera_.pitch = -limit;
        }
        dragging_ = true;
        lastX_ = x;
        lastY_ = y;
    } else {
        dragging_ = false;
    }
}

RenderFrame Scene::buildFrame(float aspect) const {
    RenderFrame frame;
    frame.view = camera_.view();
    frame.proj = camera_.proj(aspect);
    frame.cameraPos = camera_.eye();  // Phong-шейдеру нужна позиция камеры
    frame.lightDir = normalize(Vec3{0.4f, 1.0f, 0.6f});
    for (const GameObject& obj : objects_) {
        frame.items.push_back({obj.mesh, obj.material, obj.transform.matrix()});
    }

    // Анимированная модель: семплим кости на это время и кладём скин-объект.
    if (foxMesh_ != 0) {
        SkinnedItem item;
        item.mesh = foxMesh_;
        item.texture = foxTex_;
        // С текстурой цвет белый (не тонируем); без неё — рыжий запасной.
        item.color = foxTex_ != 0 ? Vec3{1.0f, 1.0f, 1.0f} : Vec3{0.85f, 0.5f, 0.25f};
        item.model = Mat4::translation({0.0f, 0.0f, 3.5f})
                   * Mat4::scale({foxScale_, foxScale_, foxScale_});
        foxModel_.sampleAnimation(foxAnim_, foxTime_, item.joints);
        frame.skinned.push_back(std::move(item));
    }
    return frame;
}
