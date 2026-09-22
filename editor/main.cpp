// Редактор сцен VBase (десктоп, Windows): окно GLFW + OpenGL 3.3, орбитальная камера,
// загрузка .scene и рендер уровня в реалтайме. Скелет фазы 1 — смотреть/крутить уровень;
// пикинг/гизмо/инспектор/Save наращиваются поверх. Ядро (Scene/рендер/загрузчики)
// переиспользуется из app/src/main/cpp (как server/ и desktop/). См. docs/NEXT_STEPS «Редактор».

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
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

    OrbitCamera cam;
    int selected = -1;         // specIndex выбранного объекта (-1 = нет)
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
            selected = scene.editorPick(ro, rd);  // -1 если мимо -> снятие выделения
        }
        lmbPrev = lmb;

        RenderFrame frame = scene.renderEditor(vmat, pmat, eye);

        frame.ui = [&]() {
            // ImGuizmo — внутри кадра ImGui (после NewFrame), рисует в его draw-list.
            ImGuizmo::BeginFrame();
            ImGuizmo::SetOrthographic(false);
            ImGuizmo::SetRect(0.0f, 0.0f, ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y);
            if (selected >= 0) {
                Mat4 model;
                if (scene.editorObjectMatrix(selected, model)) {
                    Mat4 v = vmat, p = pmat;  // ImGuizmo пишет в model.m при перетаскивании
                    if (ImGuizmo::Manipulate(v.m, p.m, ImGuizmo::TRANSLATE, ImGuizmo::WORLD, model.m)) {
                        Vec3 pos{model.m[12], model.m[13], model.m[14]};  // translate-only: берём перенос
                        Vec3 curPos, rot, scale;
                        scene.editorGetTransform(selected, curPos, rot, scale);
                        scene.editorSetTransform(selected, pos, rot, scale);
                        dirty = true;
                    }
                }
                // Подсветка выделения — рамка мирового AABB (проекция 8 углов в экран).
                Vec3 mn, mx;
                if (scene.editorWorldAABB(selected, mn, mx)) {
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
                if (selected >= 0) {
                    Vec3 pos, rot, scale;
                    scene.editorGetTransform(selected, pos, rot, scale);
                    ImGui::Text("Объект #%d", selected);
                    if (ImGui::DragFloat3("Позиция", &pos.x, 0.05f)) {
                        scene.editorSetTransform(selected, pos, rot, scale);
                        dirty = true;
                    }
                    ImGui::TextDisabled("Тащи стрелки гизмо или правь позицию");
                } else {
                    ImGui::TextDisabled("Кликни объект, чтобы выбрать");
                }
                ImGui::Separator();
                ImGui::TextDisabled("Дальше: вращение/масштаб, добавить/удалить, Save");
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
