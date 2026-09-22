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
#include <memory>
#include <string>
#include <vector>

#include "imgui.h"
#include "backends/imgui_impl_glfw.h"

#include "engine/assets/AssetSource.h"
#include "engine/assets/FileAssetSource.h"
#include "engine/core/MathUtil.h"
#include "engine/core/Renderer.h"
#include "engine/render/GlRenderer.h"
#include "game/Scene.h"

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
    std::printf("Редактор: сцена %s загружена.\n", scenePath.c_str());

    OrbitCamera cam;
    double prevX = 0.0, prevY = 0.0;
    glfwGetCursorPos(window, &prevX, &prevY);
    float scrollAccum = 0.0f;
    glfwSetWindowUserPointer(window, &scrollAccum);
    glfwSetScrollCallback(window, [](GLFWwindow* w, double, double yoff) {
        *(float*)glfwGetWindowUserPointer(w) += (float)yoff;
    });

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;

        // Ввод камеры мышью (если ImGui не забрал мышь).
        double cx = 0.0, cy = 0.0;
        glfwGetCursorPos(window, &cx, &cy);
        float dx = (float)(cx - prevX), dy = (float)(cy - prevY);
        prevX = cx;
        prevY = cy;
        const bool overUi = ImGui::GetIO().WantCaptureMouse;
        if (!overUi) {
            if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) cam.orbit(dx, dy);
            else if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS ||
                     glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS)
                cam.pan(dx, dy);
            if (scrollAccum != 0.0f) cam.zoom(scrollAccum);
        }
        scrollAccum = 0.0f;

        int fbw = 0, fbh = 0;
        glfwGetFramebufferSize(window, &fbw, &fbh);
        const float aspect = fbh > 0 ? (float)fbw / (float)fbh : 1.0f;

        RenderFrame frame = scene.renderEditor(cam.view(), cam.proj(aspect), cam.eye());

        // Минимальная панель редактора (наращиваем: outliner/inspector/Save).
        frame.ui = [&]() {
            ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(300, 0), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Редактор сцен")) {
                ImGui::TextWrapped("Сцена: %s", scenePath.c_str());
                ImGui::Separator();
                ImGui::TextDisabled("ЛКМ — орбита, ПКМ/СКМ — пан, колесо — зум");
                ImGui::Text("Камера: dist %.1f  yaw %.2f  pitch %.2f", (double)cam.distance,
                            (double)cam.yaw, (double)cam.pitch);
                if (ImGui::Button("Сбросить камеру")) cam = OrbitCamera{};
                ImGui::Separator();
                ImGui::TextDisabled("Дальше: пикинг, гизмо, инспектор, Save");
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
