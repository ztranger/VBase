// Десктопный клиент VBase (Windows): окно GLFW + рендер-бэкенд на выбор —
// OpenGL 3.3 (по умолчанию) или Vulkan (флаг --vk). Общий игровой слой Scene,
// как и на Android. Ввод — клавиатура WASD. Подключается к серверу.

#include "VulkanRenderer.h"  // тянет vulkan.h ДО GLFW (чтобы GLFW увидел VK-типы)

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "imgui.h"
#include "backends/imgui_impl_glfw.h"

#include "FileAssetSource.h"
#include "GameUi.h"
#include "GlRenderer.h"
#include "MathUtil.h"
#include "Net.h"
#include "Scene.h"

namespace {

constexpr float kTick = 1.0f / 30.0f;

int keyAxis(GLFWwindow* w, int pos, int neg) {
    int v = 0;
    if (glfwGetKey(w, pos) == GLFW_PRESS) v += 1;
    if (glfwGetKey(w, neg) == GLFW_PRESS) v -= 1;
    return v;
}

// Vulkan-ветка (Фаза 0): тот же цикл симуляции, но без ImGui/HUD и без
// glfwSwapBuffers — Vulkan презентует сам. Окно создано с GLFW_NO_API.
int runVulkan(GLFWwindow* window, const std::string& assetsDir,
              const char* serverIp, const char* scenePath) {
    FileAssetSource assets(assetsDir);
    VulkanRenderer renderer;
    // Окно передаём как непрозрачный указатель (на десктопе это GLFWwindow*).
    if (!renderer.init((ANativeWindow*)window, nullptr, assets)) {
        std::fprintf(stderr, "VulkanRenderer.init failed\n");
        return 1;
    }
    // Platform-бэкенд ImGui (контекст уже создан в renderer.init).
    ImGui_ImplGlfw_InitForVulkan(window, true);
    Scene scene;
    scene.build(renderer, assets, scenePath);
    scene.joinGame(serverIp);
    GameUiState ui;
    std::printf("Клиент запущен (Vulkan), сервер %s, сцена %s. WASD — движение, ESC — выход.\n",
                serverIp, scenePath);

    auto last = std::chrono::steady_clock::now();
    float accumulator = 0.0f;
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;

        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        if (dt > 0.25f) dt = 0.25f;

        // WASD -> ось движения; если ImGui захватил клавиатуру — не двигаемся.
        float jx = 0.0f, jy = 0.0f;
        if (!ImGui::GetIO().WantCaptureKeyboard) {
            jx = (float)keyAxis(window, GLFW_KEY_D, GLFW_KEY_A);
            jy = (float)keyAxis(window, GLFW_KEY_W, GLFW_KEY_S);
        }
        scene.setMoveInput(jx, jy);

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

        // FPS в HUD (как на GL/Android).
        if (dt > 0.0f) ui.fps = ui.fps * 0.92f + (1.0f / dt) * 0.08f;
        char hud[32];
        std::snprintf(hud, sizeof(hud), "FPS: %.0f", (double)ui.fps);
        frame.hud.push_back({hud, 24.0f, 24.0f, 40.0f, {1.0f, 0.85f, 0.2f}});

        // Панель — общий модуль GameUi (тот же, что на GL/Android).
        frame.ui = [&ui, &scene]() { GameUi::build(ui, scene); };

        // Platform-бэкенд ImGui: подать ввод/размер — до ImGui::NewFrame() в renderFrame.
        ImGui_ImplGlfw_NewFrame();

        renderer.setSurfaceSize(fbw, fbh);
        renderer.renderFrame(frame);  // Vulkan сам презентует (без glfwSwapBuffers)
    }
    scene.leaveGame();
    // Platform-бэкенд гасим до ~VulkanRenderer (renderer-бэкенд + ImGui-контекст).
    ImGui_ImplGlfw_Shutdown();
    return 0;  // ~VulkanRenderer здесь, до glfwDestroyWindow
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    const char* serverIp = (argc > 1) ? argv[1] : "127.0.0.1";
    std::string assetsDir = (argc > 2) ? argv[2] : "../../app/src/main/assets";
    const char* scenePath = (argc > 3) ? argv[3] : "scenes/default.scene";

    // Выбор бэкенда рендера: по умолчанию GL, --vk где-либо в аргументах -> Vulkan.
    bool useVk = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--vk") == 0) useVk = true;
    }

    if (!glfwInit()) {
        std::fprintf(stderr, "glfwInit failed\n");
        return 1;
    }
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
        glfwTerminate();
        return 1;
    }

    // Vulkan-ветка: свой цикл (Фаза 0). GL-ветка ниже — без изменений.
    if (useVk) {
        int rc = runVulkan(window, assetsDir, serverIp, scenePath);
        glfwDestroyWindow(window);
        glfwTerminate();
        return rc;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    int exitCode = 0;
    // Внутренняя область видимости: деструктор GlRenderer (сносит ImGui-контекст
    // и GL-объекты) должен отработать ДО glfwTerminate, пока GL-контекст ещё есть.
    {
        // Источник ассетов нужен и рендеру (шейдеры), и сцене (модели/текстуры).
        FileAssetSource assets(assetsDir);
        GlRenderer renderer;
        // Контекст уже текущий (GLFW); передаём загрузчик адресов GL-функций.
        if (!renderer.init(nullptr, [](const char* n) { return (void*)glfwGetProcAddress(n); },
                           assets)) {
            std::fprintf(stderr, "renderer.init failed\n");
            exitCode = 1;
        } else {
            // GlRenderer::init уже создал ImGui-контекст и renderer-бэкенд
            // (imgui_impl_opengl3). Добавляем platform-бэкенд GLFW: мышь,
            // клавиатура, скролл, курсоры, буфер обмена. true — ставим цепочечные
            // GLFW-колбэки (наш поллинг WASD работает независимо от них).
            ImGui_ImplGlfw_InitForOpenGL(window, true);

            Scene scene;
            scene.build(renderer, assets, scenePath);
            scene.joinGame(serverIp);  // подключиться к серверу (если запущен)
            GameUiState ui;
            std::printf("Клиент запущен, сервер %s, сцена %s. WASD — движение, ESC — выход.\n",
                        serverIp, scenePath);

            auto last = std::chrono::steady_clock::now();
            float accumulator = 0.0f;

            while (!glfwWindowShouldClose(window)) {
                glfwPollEvents();
                if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;

                auto now = std::chrono::steady_clock::now();
                float dt = std::chrono::duration<float>(now - last).count();
                last = now;
                if (dt > 0.25f) dt = 0.25f;

                // Клавиатура -> внешняя ось движения (тот же путь, что тач-джойстик).
                // Если ImGui захватил клавиатуру (курсор в поле ввода IP) — не двигаемся.
                float jx = 0.0f, jy = 0.0f;
                if (!ImGui::GetIO().WantCaptureKeyboard) {
                    jx = (float)keyAxis(window, GLFW_KEY_D, GLFW_KEY_A);
                    jy = (float)keyAxis(window, GLFW_KEY_W, GLFW_KEY_S);
                }
                scene.setMoveInput(jx, jy);

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

                // Свет задаёт сцена (из файла), правится слайдером в GameUi.

                // FPS — забота приложения (тайминг здесь), а не игровой логики.
                if (dt > 0.0f) ui.fps = ui.fps * 0.92f + (1.0f / dt) * 0.08f;
                char hud[32];
                std::snprintf(hud, sizeof(hud), "FPS: %.0f", (double)ui.fps);
                frame.hud.push_back({hud, 24.0f, 24.0f, 40.0f, {1.0f, 0.85f, 0.2f}});

                // Панель строит общий модуль GameUi (тот же, что на Android).
                frame.ui = [&ui, &scene]() { GameUi::build(ui, scene); };

                // Platform-бэкенд ImGui: подать ввод/размер/dt. Обязан идти до
                // ImGui::NewFrame(), который вызывает renderer.renderFrame().
                ImGui_ImplGlfw_NewFrame();

                renderer.setSurfaceSize(fbw, fbh);
                renderer.renderFrame(frame);

                glfwSwapBuffers(window);
            }

            scene.leaveGame();
            // Platform-бэкенд гасим до разрушения GlRenderer (его деструктор
            // делает ImGui_ImplOpenGL3_Shutdown + ImGui::DestroyContext).
            ImGui_ImplGlfw_Shutdown();
        }
    }  // <- здесь ~GlRenderer, GL-контекст ещё текущий

    glfwDestroyWindow(window);
    glfwTerminate();
    return exitCode;
}
