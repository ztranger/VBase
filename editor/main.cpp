// Редактор сцен VBase (десктоп, Windows): окно GLFW + OpenGL 3.3, орбитальная камера,
// загрузка .scene и рендер уровня в реалтайме. Скелет фазы 1 — смотреть/крутить уровень;
// пикинг/гизмо/инспектор/Save наращиваются поверх. Ядро (Scene/рендер/загрузчики)
// переиспользуется из app/src/main/cpp (как server/ и desktop/). См. docs/NEXT_STEPS «Редактор».

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "imgui.h"
#include "ImGuizmo.h"
#include "backends/imgui_impl_glfw.h"

#include "engine/assets/AssetSource.h"
#include "engine/assets/FileAssetSource.h"
#include "engine/core/MathUtil.h"
#include "engine/core/Renderer.h"
#include "engine/render/GlRenderer.h"
#include "game/GameTypes.h"  // EntityType (типы зданий)
#include "game/Scene.h"
#include "game/SceneDesc.h"
#include "game/SceneLoader.h"

namespace {

// Орбитальная камера редактора: смотрит на target, вращается вокруг (yaw/pitch), приближается
// (distance). Мышь: ЛКМ-драг — орбита, СКМ/ПКМ-драг — пан target, колесо — зум.
struct OrbitCamera {
    Vec3 target{0.0f, 0.0f, 0.0f};
    float yaw = 0.7f;       // азимут, рад
    float pitch = 0.9f;     // наклон над горизонтом, рад (0..~1.5)
    float distance = 30.0f; // дистанция до target

    Vec3 eye() const {
        float cp = std::cos(pitch), sp = std::sin(pitch);
        return target + Vec3{std::sin(yaw) * cp * distance, sp * distance,
                             std::cos(yaw) * cp * distance};
    }
    Mat4 view() const { return Mat4::lookAt(eye(), target, Vec3{0.0f, 1.0f, 0.0f}); }
    Mat4 proj(float aspect) const { return Mat4::perspective(0.9f, aspect, 0.1f, 500.0f); }

    void orbit(float dx, float dy) {
        yaw -= dx * 0.008f;
        pitch += dy * 0.008f;
        if (pitch < 0.05f) pitch = 0.05f;      // не заваливаемся за полюса
        if (pitch > 1.52f) pitch = 1.52f;
    }
    void pan(float dx, float dy) {
        // Сдвиг target в плоскости экрана (по right/forward-проекции на землю), масштаб от дистанции.
        float s = distance * 0.0016f;
        Vec3 right{std::cos(yaw), 0.0f, -std::sin(yaw)};
        Vec3 fwd{std::sin(yaw), 0.0f, std::cos(yaw)};
        target = target - right * (dx * s) + fwd * (dy * s);
    }
    void zoom(float wheel) {
        distance *= std::pow(0.9f, wheel);
        if (distance < 2.0f) distance = 2.0f;
        if (distance > 300.0f) distance = 300.0f;
    }
};

const char* buildingTypeName(int t) {
    switch ((EntityType)t) {
        case EntityType::Core:      return "Ядро";
        case EntityType::Generator: return "Генератор";
        case EntityType::Storage:   return "Склад";
        case EntityType::Spawner:   return "Спавнер";
        case EntityType::Tower:     return "Башня";
        default:                    return "?";
    }
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // Headless-проверка Save-пути (без окна): load -> serialize -> запись файла -> reload ->
    // сверка (идемпотентно + счётчики). Запускать из editor/build. Живой sync трансформов
    // (editorGetTransform) сюда не входит — он тривиален и покрыт компиляцией/[SceneRoundTrip].
    if (argc > 1 && std::string(argv[1]) == "--savetest") {
        const std::string ad = "../../app/src/main/assets";
        FileAssetSource a(ad);
        SceneDesc d;
        if (!loadSceneDesc(a, "scenes/default.scene", d)) {
            std::printf("[SaveTest] FAIL: не загрузил scenes/default.scene\n");
            return 1;
        }
        const std::string t1 = serializeSceneDesc(d);
        const std::string outRel = "scenes/_savetest.scene";
        const std::string outFull = ad + "/" + outRel;
        {
            std::ofstream f(outFull, std::ios::binary);
            if (!f.is_open()) { std::printf("[SaveTest] FAIL: не открыл %s на запись\n", outFull.c_str()); return 1; }
            f.write(t1.data(), (std::streamsize)t1.size());
        }
        SceneDesc d2;
        const bool reok = loadSceneDesc(a, outRel.c_str(), d2);
        const std::string t2 = reok ? serializeSceneDesc(d2) : std::string();
        std::remove(outFull.c_str());
        const bool ok = reok && t1 == t2 && d.objects.size() == d2.objects.size();
        std::printf("[SaveTest] %s (obj=%u, %u байт)\n", ok ? "OK" : "FAIL",
                    (unsigned)d2.objects.size(), (unsigned)t1.size());
        return ok ? 0 : 1;
    }

    // Headless-проверка гизмо-математики: извлечение Y-угла/масштаба из матрицы (как в интерактиве)
    // — точный фикспойнт к Transform::matrix() на всём диапазоне углов (в т.ч. >90°, где euler-
    // decompose клампит). M0 = T*Ry(θ)*S -> извлечь pos/yaw/scale -> собрать M1 -> M1≈M0.
    if (argc > 1 && std::string(argv[1]) == "--giztest") {
        const float D2R = 3.14159265358979f / 180.0f;
        float maxErr = 0.0f;
        const float angles[] = {0.0f, 30.0f, 90.0f, 137.0f, 200.0f, 270.0f, 350.0f};
        const float scales[] = {0.5f, 1.0f, 2.5f};
        for (float th : angles)
            for (float s : scales) {
                Transform t0;
                t0.position = {1.0f, 2.0f, 3.0f};
                t0.rotation = {0.0f, th * D2R, 0.0f};
                t0.scale = {s, s, s};
                Mat4 M0 = t0.matrix();
                const float* m = M0.m;
                Transform t1;
                t1.position = {m[12], m[13], m[14]};
                t1.rotation = {0.0f, std::atan2(-m[2], m[0]), 0.0f};
                float su = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
                t1.scale = {su, su, su};
                Mat4 M1 = t1.matrix();
                for (int i = 0; i < 16; ++i) {
                    float e = std::fabs(M1.m[i] - M0.m[i]);
                    if (e > maxErr) maxErr = e;
                }
            }
        const bool ok = maxErr < 1e-3f;
        std::printf("[GizTest] %s maxErr=%.6f\n", ok ? "OK" : "FAIL", (double)maxErr);
        return ok ? 0 : 1;
    }

    // Аргументы: [assetsDir] [scenePath] (как у desktop, но без serverIp — сети нет).
    std::string assetsDir = (argc > 1) ? argv[1] : "../../app/src/main/assets";
    std::string scenePath = (argc > 2) ? argv[2] : "scenes/default.scene";

    if (!glfwInit()) {
        std::fprintf(stderr, "glfwInit failed\n");
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    GLFWwindow* window = glfwCreateWindow(1280, 720, "VBase Scene Editor", nullptr, nullptr);
    if (window == nullptr) {
        std::fprintf(stderr, "glfwCreateWindow failed\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    FileAssetSource assets(assetsDir);
    std::unique_ptr<Renderer> renderer = std::make_unique<GlRenderer>(
        [](const char* n) { return (void*)glfwGetProcAddress(n); });
    if (!renderer->init(nullptr, assets)) {
        std::fprintf(stderr, "GlRenderer.init failed\n");
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }
    ImGui_ImplGlfw_InitForOpenGL(window, true);  // GlRenderer::init уже создал контекст + шрифт (GameUi)

    Scene scene;
    scene.build(*renderer, assets, scenePath.c_str());
    // Сырой SceneDesc (до applyBuildingConfig) — источник для Save: сериализуем именно его,
    // подтянув живые трансформы объектов из Scene. Индексы объектов совпадают со Scene.
    SceneDesc doc;
    const bool docOk = loadSceneDesc(assets, scenePath.c_str(), doc);
    std::printf("Редактор: сцена %s загружена%s.\n", scenePath.c_str(),
                docOk ? "" : " (СЫРОЙ ПАРС НЕ УДАЛСЯ — Save отключён)");

    // Браузер ассетов: все .glb/.gltf под assets/models/ (относительные пути для директивы object).
    std::vector<std::string> glbFiles;
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path root = fs::path(assetsDir) / "models";
        if (fs::exists(root, ec)) {
            for (fs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
                if (ec) break;
                if (!it->is_regular_file(ec)) continue;
                std::string ext = it->path().extension().string();
                for (char& c : ext) c = (char)std::tolower((unsigned char)c);
                if (ext != ".glb" && ext != ".gltf") continue;
                glbFiles.push_back(fs::relative(it->path(), fs::path(assetsDir), ec).generic_string());
            }
        }
        std::sort(glbFiles.begin(), glbFiles.end());
        std::printf("Редактор: найдено %u моделей в assets/models/\n", (unsigned)glbFiles.size());
    }

    OrbitCamera cam;
    // Выделение: категория + индекс. Объект — specIndex в doc.objects; здание — индекс в
    // doc.buildings (== sceneDesc_.buildings); спавн — индекс в doc.spawns.
    enum class SelKind { None, Object, Building, Spawn };
    SelKind selKind = SelKind::None;
    int selIdx = -1;
    auto deselect = [&]() { selKind = SelKind::None; selIdx = -1; };
    // Режим гизмо. Вращение — только Y (сцены/формат используют Y-рот; полный 3-осевой euler
    // разошёлся бы с порядком Transform::matrix() -> дрейф). Масштаб — равномерный (SCALEU),
    // т.к. ObjectSpec.scale — одно число. Для зданий/спавнов гизмо всегда TRANSLATE.
    ImGuizmo::OPERATION gizmoOp = ImGuizmo::TRANSLATE;
    bool snapOn = false;                                    // снап гизмо к шагу
    float snapMove = doc.grid.cell > 0.0f ? doc.grid.cell : 1.0f;  // шаг перемещения = клетка сетки
    const float snapRot = 15.0f;                            // шаг вращения, градусы
    const float snapScale = 0.25f;                          // шаг масштаба
    bool dirty = false;        // есть несохранённые правки
    std::string saveMsg;       // статус последнего Save (для панели)
    const std::string scenaFull = assetsDir + "/" + scenePath;  // куда пишем при Save

    // Save: подтянуть живые трансформы объектов в сырой doc и сериализовать в .scene-файл.
    auto saveScene = [&]() {
        if (!docOk) { saveMsg = "Save недоступен: сырой парс не удался"; return; }
        for (size_t i = 0; i < doc.objects.size(); ++i) {
            Vec3 pos, rot, scale;
            if (scene.editorGetTransform((int)i, pos, rot, scale)) {  // false для кольца/не-объектов
                doc.objects[i].pos = pos;
                doc.objects[i].rot = rot;
                doc.objects[i].scale = scale.x;  // объекты строятся с равномерным масштабом
            }
        }
        const std::string text = serializeSceneDesc(doc);
        std::ofstream f(scenaFull, std::ios::binary);  // binary: не трогать переводы строк / UTF-8
        if (f.is_open()) {
            f.write(text.data(), (std::streamsize)text.size());
            f.close();
            dirty = false;
            saveMsg = "Сохранено: " + scenaFull;
            std::printf("Редактор: сохранено %s (%u байт)\n", scenaFull.c_str(), (unsigned)text.size());
        } else {
            saveMsg = "Ошибка записи: " + scenaFull;
        }
    };

    // Добавить объект по готовому ObjectSpec — в doc и в живую сцену (с его rot/scale). Выделяет его.
    auto addObjectSpec = [&](const ObjectSpec& os) {
        const int idx = (int)doc.objects.size();
        if (scene.editorAddObjectModel(*renderer, assets, os.model, os.modelTex, os.shader, os.pos,
                                       idx) < 0)
            return;  // не загрузилась — doc не трогаем
        doc.objects.push_back(os);
        scene.editorSetTransform(idx, os.pos, os.rot, {os.scale, os.scale, os.scale});  // rot/scale копии
        selKind = SelKind::Object;
        selIdx = idx;
        dirty = true;
    };

    // Добавить свежую модель в позицию (обычно фокус камеры).
    auto addObject = [&](const std::string& model, const Vec3& pos) {
        ObjectSpec os;
        os.model = model;
        os.pos = pos;
        os.scale = 1.0f;
        os.shader = ShaderType::Lit;
        addObjectSpec(os);
    };

    // Добавить здание типа type в позицию (в doc.buildings и живую сцену, 1:1). Выделяет.
    auto addBuilding = [&](int type, const Vec3& pos) {
        const int idx = scene.editorAddBuilding(type, pos, 0);
        BuildingSpec b;
        b.kind = (BuildingSpec::Kind)(
            type == (int)EntityType::Generator ? BuildingSpec::Generator
            : type == (int)EntityType::Storage ? BuildingSpec::Storage
            : type == (int)EntityType::Spawner ? BuildingSpec::Spawner
            : type == (int)EntityType::Tower   ? BuildingSpec::Tower
                                               : BuildingSpec::Core);
        b.pos = pos;
        doc.buildings.push_back(b);
        selKind = SelKind::Building;
        selIdx = idx;
        dirty = true;
    };

    // Добавить точку спавна.
    auto addSpawn = [&](const Vec3& pos, int team) {
        const int idx = scene.editorAddSpawn(pos, team);
        SpawnSpec s;
        s.pos = pos;
        s.team = (uint8_t)team;
        doc.spawns.push_back(s);
        selKind = SelKind::Spawn;
        selIdx = idx;
        dirty = true;
    };

    // Удалить выделенное — из doc и живой сцены (индексы 1:1 в своей категории).
    auto deleteSelected = [&]() {
        if (selKind == SelKind::Object && selIdx >= 0 && selIdx < (int)doc.objects.size()) {
            scene.editorRemoveObject(selIdx);
            doc.objects.erase(doc.objects.begin() + selIdx);
        } else if (selKind == SelKind::Building && selIdx >= 0 && selIdx < (int)doc.buildings.size()) {
            scene.editorRemoveBuilding(selIdx);
            doc.buildings.erase(doc.buildings.begin() + selIdx);
        } else if (selKind == SelKind::Spawn && selIdx >= 0 && selIdx < (int)doc.spawns.size()) {
            scene.editorRemoveSpawn(selIdx);
            doc.spawns.erase(doc.spawns.begin() + selIdx);
        } else {
            return;
        }
        deselect();
        dirty = true;
    };

    // Дублировать выбранный ОБЪЕКТ: модель/шейдер/цвет из doc, ЖИВОЙ трансформ из Scene, со сдвигом.
    auto duplicateSelected = [&]() {
        if (selKind != SelKind::Object || selIdx < 0 || selIdx >= (int)doc.objects.size()) return;
        if (doc.objects[selIdx].model.empty()) return;  // дублируем только glTF-модели
        Vec3 pos, rot, scale;
        if (!scene.editorGetTransform(selIdx, pos, rot, scale)) return;
        ObjectSpec os = doc.objects[selIdx];  // model/tex/shader/color/spin
        os.pos = {pos.x + (snapOn ? snapMove : 1.0f), pos.y, pos.z};
        os.rot = rot;
        os.scale = scale.x;
        addObjectSpec(os);
    };
    double prevX = 0.0, prevY = 0.0;
    glfwGetCursorPos(window, &prevX, &prevY);
    bool lmbPrev = false;
    double downX = 0.0, downY = 0.0;
    bool dragMoved = false;
    float scrollAccum = 0.0f;
    glfwSetWindowUserPointer(window, &scrollAccum);
    glfwSetScrollCallback(window, [](GLFWwindow* w, double, double yoff) {
        *(float*)glfwGetWindowUserPointer(w) += (float)yoff;
    });

    // Луч из точки экрана (пиксели) в мир: unproject NDC через inverse(proj*view).
    auto screenRay = [](const Mat4& view, const Mat4& proj, const Vec3& eye, float mx, float my,
                        float w, float h, Vec3& ro, Vec3& rd) {
        const float nx = 2.0f * mx / w - 1.0f, ny = 1.0f - 2.0f * my / h;
        const Mat4 inv = inverse(proj * view);
        auto un = [&](float z) {
            const float* m = inv.m;
            float x = m[0] * nx + m[4] * ny + m[8] * z + m[12];
            float y = m[1] * nx + m[5] * ny + m[9] * z + m[13];
            float zz = m[2] * nx + m[6] * ny + m[10] * z + m[14];
            float ww = m[3] * nx + m[7] * ny + m[11] * z + m[15];
            if (std::fabs(ww) < 1e-8f) ww = 1e-8f;
            return Vec3{x / ww, y / ww, zz / ww};
        };
        ro = eye;
        rd = normalize(un(1.0f) - eye);  // к дальней плоскости
    };

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;

        // Ctrl+S -> Save (по фронту, если ImGui не забрал клавиатуру).
        static bool sPrev = false;
        const bool sNow = !ImGui::GetIO().WantCaptureKeyboard &&
                          (glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
                           glfwGetKey(window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS) &&
                          glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS;
        if (sNow && !sPrev) saveScene();
        sPrev = sNow;

        // W/E/R — режим гизмо (перемещение/вращение Y/масштаб), если ImGui не забрал клавиатуру.
        if (!ImGui::GetIO().WantCaptureKeyboard) {
            if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) gizmoOp = ImGuizmo::TRANSLATE;
            else if (glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS) gizmoOp = ImGuizmo::ROTATE_Y;
            else if (glfwGetKey(window, GLFW_KEY_R) == GLFW_PRESS) gizmoOp = ImGuizmo::SCALEU;
        }
        // Delete — удалить выделенное (по фронту).
        static bool delPrev = false;
        const bool delNow = !ImGui::GetIO().WantCaptureKeyboard && selKind != SelKind::None &&
                            glfwGetKey(window, GLFW_KEY_DELETE) == GLFW_PRESS;
        if (delNow && !delPrev) deleteSelected();
        delPrev = delNow;

        // Ctrl+D — дублировать выбранный объект (по фронту).
        static bool dupPrev = false;
        const bool dupNow = !ImGui::GetIO().WantCaptureKeyboard && selKind == SelKind::Object &&
                           (glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
                            glfwGetKey(window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS) &&
                           glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS;
        if (dupNow && !dupPrev) duplicateSelected();
        dupPrev = dupNow;

        double cx = 0.0, cy = 0.0;
        glfwGetCursorPos(window, &cx, &cy);
        float dx = (float)(cx - prevX), dy = (float)(cy - prevY);
        prevX = cx;
        prevY = cy;

        int fbw = 0, fbh = 0;
        glfwGetFramebufferSize(window, &fbw, &fbh);
        const float aspect = fbh > 0 ? (float)fbw / (float)fbh : 1.0f;
        const Mat4 vmat = cam.view();
        const Mat4 pmat = cam.proj(aspect);
        const Vec3 eye = cam.eye();

        const bool overUi = ImGui::GetIO().WantCaptureMouse;
        // Гизмо перехватывает мышь (состояние прошлого кадра) — тогда камеру/пикинг не трогаем.
        const bool gizmo = ImGuizmo::IsUsing() || ImGuizmo::IsOver();

        // Камера: ЛКМ-драг — орбита (если не над UI/гизмо), ПКМ/СКМ — пан, колесо — зум.
        if (!overUi && !gizmo &&
            glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS)
            cam.orbit(dx, dy);
        if (!overUi &&
            (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS ||
             glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS))
            cam.pan(dx, dy);
        if (!overUi && scrollAccum != 0.0f) cam.zoom(scrollAccum);
        scrollAccum = 0.0f;

        // Пикинг: ЛКМ-клик (без драга, не над UI/гизмо) — луч в сцену -> выбранный объект.
        const bool lmb = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        if (lmb && !lmbPrev) { downX = cx; downY = cy; dragMoved = false; }
        if (lmb && (std::fabs(cx - downX) + std::fabs(cy - downY) > 4.0)) dragMoved = true;
        if (!lmb && lmbPrev && !dragMoved && !overUi && !ImGuizmo::IsOver()) {
            Vec3 ro, rd;
            screenRay(vmat, pmat, eye, (float)cx, (float)cy, (float)fbw, (float)fbh, ro, rd);
            // Ближайшее среди объектов / зданий / спавнов.
            float best = 1e30f;
            deselect();
            int i;
            float t;
            if (scene.editorPickObject(ro, rd, i, t) && t < best) { best = t; selKind = SelKind::Object; selIdx = i; }
            if (scene.editorPickBuilding(ro, rd, i, t) && t < best) { best = t; selKind = SelKind::Building; selIdx = i; }
            if (scene.editorPickSpawn(ro, rd, i, t) && t < best) { best = t; selKind = SelKind::Spawn; selIdx = i; }
        }
        lmbPrev = lmb;

        RenderFrame frame = scene.renderEditor(vmat, pmat, eye);

        frame.ui = [&]() {
            // ImGuizmo — внутри кадра ImGui (после NewFrame), рисует в его draw-list.
            ImGuizmo::BeginFrame();
            ImGuizmo::SetOrthographic(false);
            ImGuizmo::SetRect(0.0f, 0.0f, ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y);
            if (selKind != SelKind::None) {
                // Модельная матрица + операция гизмо по типу выделения. Объект — полный W/E/R;
                // здание/спавн — только перемещение (позиционные сущности).
                Mat4 model = Mat4::translation({0.0f, 0.0f, 0.0f});
                bool haveModel = false;
                ImGuizmo::OPERATION op = gizmoOp;
                if (selKind == SelKind::Object) {
                    haveModel = scene.editorObjectMatrix(selIdx, model);
                } else if (selKind == SelKind::Building) {
                    int type, team; Vec3 bp;
                    if (scene.editorBuildingInfo(selIdx, type, team, bp)) {
                        model = Mat4::translation(bp); haveModel = true; op = ImGuizmo::TRANSLATE;
                    }
                } else if (selKind == SelKind::Spawn) {
                    int team; Vec3 sp;
                    if (scene.editorSpawnInfo(selIdx, team, sp)) {
                        model = Mat4::translation(sp); haveModel = true; op = ImGuizmo::TRANSLATE;
                    }
                }
                if (haveModel) {
                    Mat4 v = vmat, p = pmat;  // ImGuizmo пишет в model.m при перетаскивании
                    const ImGuizmo::MODE mode =
                        (op == ImGuizmo::TRANSLATE) ? ImGuizmo::WORLD : ImGuizmo::LOCAL;
                    const float step = (op == ImGuizmo::TRANSLATE) ? snapMove
                                       : (op == ImGuizmo::ROTATE_Y) ? snapRot : snapScale;
                    const float snap[3] = {step, step, step};
                    if (ImGuizmo::Manipulate(v.m, p.m, op, mode, model.m, nullptr,
                                             snapOn ? snap : nullptr)) {
                        const float* m = model.m;
                        const Vec3 np{m[12], m[13], m[14]};
                        if (selKind == SelKind::Object) {
                            // Извлекаем прямо из матрицы (точный обратный к T*Ry*S; полный 360° по Y).
                            Vec3 pos, rot, scale;
                            scene.editorGetTransform(selIdx, pos, rot, scale);
                            if (op == ImGuizmo::TRANSLATE) pos = np;
                            else if (op == ImGuizmo::ROTATE_Y) rot.y = std::atan2(-m[2], m[0]);
                            else {
                                float su = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
                                scale = {su < 1e-4f ? 1e-4f : su, su < 1e-4f ? 1e-4f : su,
                                         su < 1e-4f ? 1e-4f : su};
                            }
                            scene.editorSetTransform(selIdx, pos, rot, scale);
                        } else if (selKind == SelKind::Building) {
                            scene.editorSetBuildingPos(selIdx, np);
                            if (selIdx < (int)doc.buildings.size()) doc.buildings[selIdx].pos = np;
                        } else {  // Spawn
                            scene.editorSetSpawnPos(selIdx, np);
                            if (selIdx < (int)doc.spawns.size()) doc.spawns[selIdx].pos = np;
                        }
                        dirty = true;
                    }
                }
                // Подсветка выделения — рамка мирового AABB (проекция 8 углов в экран).
                Vec3 mn, mx;
                bool haveBox = (selKind == SelKind::Object)   ? scene.editorWorldAABB(selIdx, mn, mx)
                             : (selKind == SelKind::Building) ? scene.editorBuildingWorldAABB(selIdx, mn, mx)
                                                              : scene.editorSpawnWorldAABB(selIdx, mn, mx);
                if (haveBox) {
                    const Mat4 vp = pmat * vmat;
                    auto proj = [&](Vec3 wp, ImVec2& out) -> bool {
                        const float* m = vp.m;
                        float x = m[0] * wp.x + m[4] * wp.y + m[8] * wp.z + m[12];
                        float y = m[1] * wp.x + m[5] * wp.y + m[9] * wp.z + m[13];
                        float w = m[3] * wp.x + m[7] * wp.y + m[11] * wp.z + m[15];
                        if (w <= 1e-5f) return false;
                        const ImVec2 d = ImGui::GetIO().DisplaySize;
                        out = ImVec2((x / w * 0.5f + 0.5f) * d.x, (1.0f - (y / w * 0.5f + 0.5f)) * d.y);
                        return true;
                    };
                    ImVec2 c[8];
                    bool ok = true;
                    for (int i = 0; i < 8; ++i) {
                        Vec3 wp{(i & 1) ? mx.x : mn.x, (i & 2) ? mx.y : mn.y, (i & 4) ? mx.z : mn.z};
                        ok = ok && proj(wp, c[i]);
                    }
                    if (ok) {
                        ImDrawList* dl = ImGui::GetForegroundDrawList();
                        const ImU32 col = IM_COL32(232, 161, 58, 235);  // amber
                        const int edges[12][2] = {{0,1},{1,3},{3,2},{2,0},{4,5},{5,7},{7,6},{6,4},
                                                  {0,4},{1,5},{2,6},{3,7}};
                        for (auto& e : edges) dl->AddLine(c[e[0]], c[e[1]], col, 1.5f);
                    }
                }
            }

            ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(320, 0), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Редактор сцен")) {
                ImGui::TextWrapped("Сцена: %s%s", scenePath.c_str(), dirty ? " *" : "");
                if (ImGui::Button("Сохранить (Ctrl+S)")) saveScene();
                if (!saveMsg.empty()) ImGui::TextDisabled("%s", saveMsg.c_str());
                ImGui::Separator();
                ImGui::TextDisabled("ЛКМ-клик — выбрать, ЛКМ-драг — орбита, ПКМ/СКМ — пан, колесо — зум");
                ImGui::Text("Камера: dist %.1f", (double)cam.distance);
                if (ImGui::Button("Сбросить камеру")) cam = OrbitCamera{};
                ImGui::Separator();
                // Режим гизмо (W/E/R).
                int opIdx = (gizmoOp == ImGuizmo::TRANSLATE) ? 0 : (gizmoOp == ImGuizmo::ROTATE_Y ? 1 : 2);
                ImGui::TextUnformatted("Гизмо:");
                ImGui::SameLine();
                if (ImGui::RadioButton("Перемещение (W)", opIdx == 0)) gizmoOp = ImGuizmo::TRANSLATE;
                if (ImGui::RadioButton("Вращение Y (E)", opIdx == 1)) gizmoOp = ImGuizmo::ROTATE_Y;
                ImGui::SameLine();
                if (ImGui::RadioButton("Масштаб (R)", opIdx == 2)) gizmoOp = ImGuizmo::SCALEU;
                ImGui::Checkbox("Снап к сетке", &snapOn);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(90.0f);
                ImGui::DragFloat("шаг", &snapMove, 0.1f, 0.1f, 100.0f);
                ImGui::TextDisabled("снап: перемещ. — шаг, вращ. — 15°, масштаб — 0.25");
                ImGui::Separator();
                if (selKind == SelKind::Object && selIdx >= 0 && selIdx < (int)doc.objects.size()) {
                    Vec3 pos, rot, scale;
                    scene.editorGetTransform(selIdx, pos, rot, scale);
                    ImGui::Text("Объект #%d", selIdx);
                    bool ch = false;
                    ch |= ImGui::DragFloat3("Позиция", &pos.x, 0.05f);
                    float yawDeg = rot.y * (180.0f / 3.14159265358979f);
                    if (ImGui::DragFloat("Поворот Y°", &yawDeg, 1.0f)) {
                        rot.y = yawDeg * (3.14159265358979f / 180.0f);
                        ch = true;
                    }
                    float su = scale.x;
                    if (ImGui::DragFloat("Масштаб", &su, 0.01f, 0.01f, 100.0f)) {
                        scale = {su, su, su};
                        ch = true;
                    }
                    if (ch) {
                        scene.editorSetTransform(selIdx, pos, rot, scale);
                        dirty = true;
                    }
                    if (!doc.objects[selIdx].model.empty()) {  // material/shader — только для glTF-моделей
                        ObjectSpec& os = doc.objects[selIdx];
                        const char* shaders[] = {"Lit", "Unlit", "Phong"};  // порядок ShaderType
                        int sh = (int)os.shader;
                        if (ImGui::Combo("Шейдер", &sh, shaders, 3)) {
                            os.shader = (ShaderType)sh;
                            scene.editorSetObjectMaterial(*renderer, selIdx, os.shader, os.color);
                            dirty = true;
                        }
                        if (ImGui::ColorEdit3("Тинт", &os.color.x)) {
                            scene.editorSetObjectMaterial(*renderer, selIdx, os.shader, os.color);
                            dirty = true;
                        }
                    }
                    if (ImGui::Button("Дублировать (Ctrl+D)")) duplicateSelected();
                    ImGui::SameLine();
                    if (ImGui::Button("Удалить (Del)")) deleteSelected();
                } else if (selKind == SelKind::Building) {
                    int type, team; Vec3 bp;
                    if (scene.editorBuildingInfo(selIdx, type, team, bp)) {
                        ImGui::Text("Здание #%d — %s", selIdx, buildingTypeName(type));
                        if (ImGui::DragFloat3("Позиция", &bp.x, 0.05f)) {
                            scene.editorSetBuildingPos(selIdx, bp);
                            if (selIdx < (int)doc.buildings.size()) doc.buildings[selIdx].pos = bp;
                            dirty = true;
                        }
                        if (ImGui::DragInt("Команда", &team, 0.05f, 0, 2)) {
                            scene.editorSetBuildingTeam(selIdx, team);
                            if (selIdx < (int)doc.buildings.size()) doc.buildings[selIdx].team = (uint8_t)team;
                            dirty = true;
                        }
                        if (ImGui::Button("Удалить (Del)")) deleteSelected();
                        ImGui::TextDisabled("Параметры (hp/rate/…) — из config/buildings.cfg");
                    }
                } else if (selKind == SelKind::Spawn) {
                    int team; Vec3 sp;
                    if (scene.editorSpawnInfo(selIdx, team, sp)) {
                        ImGui::Text("Точка спавна #%d", selIdx);
                        if (ImGui::DragFloat3("Позиция", &sp.x, 0.05f)) {
                            scene.editorSetSpawnPos(selIdx, sp);
                            if (selIdx < (int)doc.spawns.size()) doc.spawns[selIdx].pos = sp;
                            dirty = true;
                        }
                        if (ImGui::DragInt("Команда", &team, 0.05f, 0, 2)) {
                            scene.editorSetSpawnTeam(selIdx, team);
                            if (selIdx < (int)doc.spawns.size()) doc.spawns[selIdx].team = (uint8_t)team;
                            dirty = true;
                        }
                        if (ImGui::Button("Удалить (Del)")) deleteSelected();
                    }
                } else {
                    ImGui::TextDisabled("Кликни объект / здание / спавн, чтобы выбрать");
                }
                ImGui::Separator();
                ImGui::TextUnformatted("Добавить в фокус камеры:");
                if (ImGui::Button("Ядро")) addBuilding((int)EntityType::Core, cam.target);
                ImGui::SameLine();
                if (ImGui::Button("Генератор")) addBuilding((int)EntityType::Generator, cam.target);
                ImGui::SameLine();
                if (ImGui::Button("Склад")) addBuilding((int)EntityType::Storage, cam.target);
                if (ImGui::Button("Спавнер")) addBuilding((int)EntityType::Spawner, cam.target);
                ImGui::SameLine();
                if (ImGui::Button("Башня")) addBuilding((int)EntityType::Tower, cam.target);
                ImGui::SameLine();
                if (ImGui::Button("Точка спавна")) addSpawn(cam.target, 0);
                ImGui::Separator();
                ImGui::TextUnformatted("Объект (.glb) в фокус камеры:");
                ImGui::BeginChild("glblist", ImVec2(0, 130), true);
                if (glbFiles.empty()) ImGui::TextDisabled("Нет .glb в assets/models/");
                for (const std::string& f : glbFiles)
                    if (ImGui::Selectable(f.c_str())) addObject(f, cam.target);
                ImGui::EndChild();
            }
            ImGui::End();
        };

        ImGui_ImplGlfw_NewFrame();
        renderer->setSurfaceSize(fbw, fbh);
        renderer->renderFrame(frame);
        glfwSwapBuffers(window);
    }

    ImGui_ImplGlfw_Shutdown();
    renderer.reset();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
