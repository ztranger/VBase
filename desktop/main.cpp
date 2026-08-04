// Десктопный клиент VBase (Windows): окно GLFW + рендер-бэкенд на выбор —
// OpenGL 3.3 или Vulkan. Бэкенд переключается кнопкой в панели GameUi в рантайме
// (окно и рендер пересоздаются). Общий игровой слой Scene, как и на Android.

#include "engine/render/VulkanRenderer.h"  // тянет vulkan.h ДО GLFW (чтобы GLFW увидел VK-типы)

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "imgui.h"
#include "backends/imgui_impl_glfw.h"

#include "engine/assets/FileAssetSource.h"
#include "engine/render/GameUi.h"
#include "engine/render/GlRenderer.h"
#include "engine/core/MathUtil.h"
#include "engine/net/Net.h"
#include "engine/core/Renderer.h"
#include "game/Scene.h"

namespace {

constexpr float kTick = kTickDt;  // единый шаг симуляции (из engine/net/Net.h)

int keyAxis(GLFWwindow* w, int pos, int neg) {
    int v = 0;
    if (glfwGetKey(w, pos) == GLFW_PRESS) v += 1;
    if (glfwGetKey(w, neg) == GLFW_PRESS) v -= 1;
    return v;
}

// Запустить клиент на заданном бэкенде (0 = OpenGL, 1 = Vulkan). Создаёт своё окно
// и рендер. Возвращает следующий бэкенд для перезапуска (по кнопке в GameUi) либо
// -1, если окно закрыто/ESC (выход). ui переживает переключения (свет/камера/FPS).
int runClient(int backend, GameUiState& ui, const std::string& assetsDir,
              const char* serverIp, const char* scenePath) {
    const bool useVk = (backend == 1);

    glfwDefaultWindowHints();
    if (useVk) {
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);  // Vulkan: без GL-контекста
    } else {
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    }
    GLFWwindow* window = glfwCreateWindow(1280, 720, "VBase Desktop", nullptr, nullptr);
    if (window == nullptr) {
        std::fprintf(stderr, "glfwCreateWindow failed\n");
        return -1;
    }

    FileAssetSource assets(assetsDir);
    std::unique_ptr<Renderer> renderer;
    if (useVk) {
        auto r = std::make_unique<VulkanRenderer>();
        if (!r->init((ANativeWindow*)window, nullptr, assets)) {
            std::fprintf(stderr, "VulkanRenderer.init failed — откат на OpenGL\n");
            glfwDestroyWindow(window);
            return 0;  // graceful fallback на GL
        }
        ImGui_ImplGlfw_InitForVulkan(window, true);
        renderer = std::move(r);
    } else {
        glfwMakeContextCurrent(window);
        glfwSwapInterval(1);
        auto r = std::make_unique<GlRenderer>();
        if (!r->init(nullptr, [](const char* n) { return (void*)glfwGetProcAddress(n); }, assets)) {
            std::fprintf(stderr, "GlRenderer.init failed\n");
            glfwDestroyWindow(window);
            return -1;
        }
        ImGui_ImplGlfw_InitForOpenGL(window, true);
        renderer = std::move(r);
    }

    Scene scene;
    scene.build(*renderer, assets, scenePath);
    scene.joinGame(serverIp);

    ui.backend = backend;
    ui.requestBackend = -1;  // сбросить возможный запрос предыдущего бэкенда
    std::printf("Клиент запущен (%s), сервер %s, сцена %s.\n",
                useVk ? "Vulkan" : "OpenGL", serverIp, scenePath);

    auto last = std::chrono::steady_clock::now();
    float accumulator = 0.0f;
    bool prevSpace = false;  // фронт нажатия пробела -> прыжок
    bool prevMouse = false;  // фронт клика ЛКМ -> пикинг здания
    bool prevEnter = false;  // фронт Enter -> подтвердить постройку
    int next = -1;  // -1 = выход

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;

        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        if (dt > 0.25f) dt = 0.25f;

        // Игровой ввод активен только в бою без модалки: в меню/лоадинге/под диалогом
        // герой не ходит и камера не крутится, даже если жать по пустым зонам экрана.
        const bool play = GameUi::gameplayActive();

        // WASD -> ось движения; если ImGui захватил клавиатуру — не двигаемся.
        float jx = 0.0f, jy = 0.0f;
        if (play && !ImGui::GetIO().WantCaptureKeyboard) {
            jx = (float)keyAxis(window, GLFW_KEY_D, GLFW_KEY_A);
            jy = (float)keyAxis(window, GLFW_KEY_W, GLFW_KEY_S);
        }
        scene.setMoveInput(jx, jy);

        // Стрелки -> камера: лево/право = орбита вокруг героя, верх/низ = зум.
        float cyaw = 0.0f, czoom = 0.0f;
        if (play && !ImGui::GetIO().WantCaptureKeyboard) {
            cyaw = (float)keyAxis(window, GLFW_KEY_RIGHT, GLFW_KEY_LEFT);
            czoom = (float)keyAxis(window, GLFW_KEY_UP, GLFW_KEY_DOWN);  // вверх = приблизить
        }
        scene.setCameraInput(cyaw, czoom);

        // Пробел (по фронту нажатия) -> прыжок.
        bool space = play && !ImGui::GetIO().WantCaptureKeyboard &&
                     glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS;
        if (space && !prevSpace) scene.requestJump();
        prevSpace = space;

        // Enter (по фронту) -> подтвердить постройку (в режиме стройки).
        bool enter = play && !ImGui::GetIO().WantCaptureKeyboard &&
                     glfwGetKey(window, GLFW_KEY_ENTER) == GLFW_PRESS;
        if (enter && !prevEnter) scene.confirmBuild();
        prevEnter = enter;

        // ЛКМ (по фронту, если ImGui не забрал мышь) -> пикинг здания под курсором.
        bool mouse = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        if (play && mouse && !prevMouse && !ImGui::GetIO().WantCaptureMouse) {
            double cx = 0.0, cy = 0.0;
            glfwGetCursorPos(window, &cx, &cy);
            int ww = 0, wh = 0;
            glfwGetWindowSize(window, &ww, &wh);
            scene.onClick((float)cx, (float)cy, (float)ww, (float)wh);
        }
        prevMouse = mouse;

        accumulator += dt;
        while (accumulator >= kTick) {
            scene.fixedUpdate(kTick);
            accumulator -= kTick;
        }
        float alpha = accumulator / kTick;

        int fbw = 0, fbh = 0;
        glfwGetFramebufferSize(window, &fbw, &fbh);
        float aspect = fbh > 0 ? (float)fbw / (float)fbh : 1.0f;

        RenderFrame frame = scene.render(alpha, aspect, dt);
        frame.deltaTime = dt;

        // FPS в HUD.
        if (dt > 0.0f) ui.fps = ui.fps * 0.92f + (1.0f / dt) * 0.08f;
        char hud[32];
        std::snprintf(hud, sizeof(hud), "FPS: %.0f", (double)ui.fps);
        frame.hud.push_back({hud, 24.0f, 24.0f, 40.0f, {1.0f, 0.85f, 0.2f}});

        // Панель — общий модуль GameUi (тот же, что на Android). Тут же кнопки
        // переключения бэкенда пишут ui.requestBackend.
        frame.ui = [&ui, &scene]() { GameUi::build(ui, scene); };

        // Platform-бэкенд ImGui: до ImGui::NewFrame() (его делает renderer.renderFrame).
        ImGui_ImplGlfw_NewFrame();

        renderer->setSurfaceSize(fbw, fbh);
        renderer->renderFrame(frame);
        if (!useVk) glfwSwapBuffers(window);  // Vulkan презентует сам

        // Запрошено переключение бэкенда кнопкой — выходим из цикла на перезапуск.
        if (ui.requestBackend >= 0 && ui.requestBackend != backend) {
            next = ui.requestBackend;
            break;
        }
    }

    scene.leaveGame();
    // Platform-бэкенд гасим до разрушения рендера (его деструктор сносит
    // renderer-бэкенд ImGui + контекст). Порядок важен.
    ImGui_ImplGlfw_Shutdown();
    renderer.reset();  // GL: контекст ещё текущий; VK: vkDeviceWaitIdle в деструкторе
    glfwDestroyWindow(window);
    return next;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // Аргументы: позиционные [serverIp] [assetsDir] [scenePath] + флаги (--vk / --loading)
    // в любом месте. Флаги НЕ занимают позиционные слоты — поэтому `--loading` одним
    // аргументом оставляет serverIp = 127.0.0.1 (иначе авто-joinGame виснет на DNS).
    const char* serverIp = "127.0.0.1";
    std::string assetsDir = "../../app/src/main/assets";
    const char* scenePath = "scenes/default.scene";
    int backend = 0;  // GL по умолчанию, --vk -> Vulkan; дальше — кнопки в GameUi
    bool startLoadingPreview = false;

    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--vk") == 0) {
            backend = 1;
        } else if (std::strcmp(argv[i], "--loading") == 0) {
            startLoadingPreview = true;
        } else {
            switch (positional++) {
                case 0: serverIp = argv[i]; break;
                case 1: assetsDir = argv[i]; break;
                case 2: scenePath = argv[i]; break;
                default: break;  // лишние позиционные игнорируем
            }
        }
    }

    if (!glfwInit()) {
        std::fprintf(stderr, "glfwInit failed\n");
        return 1;
    }

    const bool vulkanAvailable = (glfwVulkanSupported() == GLFW_TRUE);
    if (backend == 1 && !vulkanAvailable) backend = 0;  // запрошен --vk, но нет Vulkan

    GameUiState ui;
    ui.vulkanAvailable = vulkanAvailable;
    if (startLoadingPreview) GameUi::requestLoadingScreen();  // --loading: стартуем на лоадинге

    // Цикл перезапуска: runClient возвращает следующий бэкенд или -1 (выход).
    while (backend >= 0) {
        backend = runClient(backend, ui, assetsDir, serverIp, scenePath);
    }

    glfwTerminate();
    return 0;
}
