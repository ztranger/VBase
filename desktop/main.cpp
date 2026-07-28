// Десктопный клиент VBase (Windows): окно GLFW + desktop OpenGL 3.3, тот же
// кроссплатформенный GlRenderer и общий игровой слой Scene, что и на Android.
// Полный рендер (сцена, лиса со скиннингом, HUD, ImGui). Ввод — клавиатура WASD.
// Подключается к серверу, синхронизирует игроков.

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

#include "FileAssetSource.h"
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

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    const char* serverIp = (argc > 1) ? argv[1] : "127.0.0.1";
    std::string assetsDir = (argc > 2) ? argv[2] : "../../app/src/main/assets";

    if (!glfwInit()) {
        std::fprintf(stderr, "glfwInit failed\n");
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    GLFWwindow* window = glfwCreateWindow(1280, 720, "VBase Desktop", nullptr, nullptr);
    if (window == nullptr) {
        std::fprintf(stderr, "glfwCreateWindow failed\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    GlRenderer renderer;
    // Контекст уже текущий (GLFW); передаём загрузчик адресов GL-функций.
    if (!renderer.init(nullptr, [](const char* n) { return (void*)glfwGetProcAddress(n); })) {
        std::fprintf(stderr, "renderer.init failed\n");
        return 1;
    }

    FileAssetSource assets(assetsDir);
    Scene scene;
    scene.build(renderer, assets);
    scene.joinGame(serverIp);  // подключиться к серверу (если запущен)
    std::printf("Клиент запущен, сервер %s. WASD — движение, ESC — выход.\n", serverIp);

    auto last = std::chrono::steady_clock::now();
    float accumulator = 0.0f;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;

        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        if (dt > 0.25f) dt = 0.25f;

        // Клавиатура -> внешняя ось движения сцены (тот же путь, что тач-джойстик).
        float jx = (float)keyAxis(window, GLFW_KEY_D, GLFW_KEY_A);
        float jy = (float)keyAxis(window, GLFW_KEY_W, GLFW_KEY_S);
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
        renderer.setSurfaceSize(fbw, fbh);
        renderer.renderFrame(frame);

        glfwSwapBuffers(window);
    }

    scene.leaveGame();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
