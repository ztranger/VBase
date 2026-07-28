#include <game-activity/native_app_glue/android_native_app_glue.h>
#include <android/asset_manager.h>
#include <android/configuration.h>
#include <android/input.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "imgui.h"

#include "AssetSource.h"
#include "GameUi.h"
#include "GlRenderer.h"
#include "Log.h"
#include "RenderFrame.h"
#include "Scene.h"
#include "VulkanProbe.h"

namespace {
// Источник ассетов Android поверх AAssetManager (файлы из APK).
class AndroidAssetSource : public AssetSource {
public:
    explicit AndroidAssetSource(AAssetManager* mgr) : mgr_(mgr) {}
    bool read(const char* path, std::vector<uint8_t>& out) override {
        AAsset* a = AAssetManager_open(mgr_, path, AASSET_MODE_BUFFER);
        if (a == nullptr) return false;
        off_t len = AAsset_getLength(a);
        out.resize((size_t)len);
        int r = AAsset_read(a, out.data(), (size_t)len);
        AAsset_close(a);
        return r == (int)len;
    }
private:
    AAssetManager* mgr_;
};
}  // namespace

// NB: VulkanRenderer временно отключён от сборки, пока портируется под
// новый контракт RenderFrame (см. CMakeLists.txt). VulkanProbe оставлен,
// чтобы логировать доступность Vulkan.

namespace {

struct Engine {
    std::unique_ptr<Renderer> renderer;
    std::unique_ptr<Scene> scene;
    bool vulkanSupported = false;
    std::chrono::steady_clock::time_point lastTime{};
    bool haveTime = false;
    float accumulator = 0.0f; // накопитель времени для фиксированного тика
    float uiScale = 1.0f;    // масштаб UI по плотности экрана
    GameUiState ui;          // состояние панели (fps/угол света/IP) — общее с десктопом
};

// Частота симуляции. Рендер идёт быстрее и интерполирует между тиками.
// Именно на этом тике позже будет крутиться и сетевая симуляция.
constexpr float kTick = 1.0f / 30.0f;

void handleCmd(android_app* app, int32_t cmd) {
    auto* engine = static_cast<Engine*>(app->userData);

    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            if (app->window != nullptr) {
                auto renderer = std::make_unique<GlRenderer>();
                // Источник ассетов нужен и рендеру (шейдеры), и сцене (модели/
                // текстуры) — один на оба, живёт весь init/build.
                AndroidAssetSource assets(app->activity->assetManager);
                if (renderer->init(app->window, nullptr, assets)) {
                    engine->renderer = std::move(renderer);
                    // Мир строится после инициализации рендера: ему нужны
                    // живой GPU-контекст (залить меши/текстуры) и AAssetManager
                    // (загрузить модели/картинки из APK).
                    engine->scene = std::make_unique<Scene>();
                    engine->scene->build(*engine->renderer, assets);
                    engine->haveTime = false;

                    // Масштаб UI по плотности экрана — иначе на телефоне с высоким
                    // DPI весь интерфейс рендерится в физ. пикселях 1:1 и крошечный.
                    int density = AConfiguration_getDensity(app->config);
                    float scale = 2.5f;  // разумный дефолт, если density неопределён
                    if (density > 0 && density <= 1000) {
                        scale = (float)density / 160.0f;  // 160 dpi = mdpi baseline
                    }
                    if (scale < 1.0f) scale = 1.0f;
                    if (scale > 4.0f) scale = 4.0f;
                    engine->uiScale = scale;
                    engine->scene->setUiScale(scale);  // джойстик под DPI

                    // ImGui 1.92: FontScaleDpi даёт ЧЁТКОЕ масштабирование (шрифт
                    // ре-растеризуется), ScaleAllSizes масштабирует отступы/паддинги.
                    ImGuiStyle& style = ImGui::GetStyle();
                    style.ScaleAllSizes(scale);
                    style.FontScaleDpi = scale;
                    LOGI("UI scale: %.2f (density %d dpi)", (double)scale, density);
                } else {
                    LOGE("Renderer init failed");
                }
            }
            break;

        case APP_CMD_TERM_WINDOW:
            // Поверхность уничтожается — сносим рендер (GPU-ресурсы) и мир.
            engine->scene.reset();
            engine->renderer.reset();
            break;

        default:
            break;
    }
}

// Ввод касаний -> в игровой слой (сцена вращает камеру). Рендер к вводу не причастен.
void pumpInput(android_app* app, Scene* scene) {
    android_input_buffer* input = android_app_swap_input_buffers(app);
    if (input == nullptr) {
        return;
    }

    for (uint64_t i = 0; i < input->motionEventsCount; ++i) {
        GameActivityMotionEvent& event = input->motionEvents[i];
        const int actionMasked = event.action & AMOTION_EVENT_ACTION_MASK;

        bool pressed;
        switch (actionMasked) {
            case AMOTION_EVENT_ACTION_DOWN:
            case AMOTION_EVENT_ACTION_POINTER_DOWN:
            case AMOTION_EVENT_ACTION_MOVE:
                pressed = true;
                break;
            case AMOTION_EVENT_ACTION_UP:
            case AMOTION_EVENT_ACTION_POINTER_UP:
            case AMOTION_EVENT_ACTION_CANCEL:
                pressed = false;
                break;
            default:
                continue;
        }

        if (event.pointerCount > 0) {
            const float x = GameActivityPointerAxes_getX(&event.pointers[0]);
            const float y = GameActivityPointerAxes_getY(&event.pointers[0]);

            // Касание -> ImGui как мышь.
            ImGuiIO& io = ImGui::GetIO();
            io.AddMousePosEvent(x, y);
            io.AddMouseButtonEvent(0, pressed);

            // Если ImGui «съел» касание (палец на виджете) — не крутим камеру.
            if (scene != nullptr && !io.WantCaptureMouse) {
                scene->onPointer(x, y, pressed);
            }
        }
    }
    android_app_clear_motion_events(input);

    if (input->keyEventsCount > 0) {
        android_app_clear_key_events(input);
    }
}

} // namespace

extern "C" void android_main(android_app* app) {
    LOGI("android_main started");

    Engine engine;
    engine.vulkanSupported = VulkanProbe::isSupported();
    LOGI("Vulkan supported: %s", engine.vulkanSupported ? "yes" : "no");

    app->userData = &engine;
    app->onAppCmd = handleCmd;

    while (!app->destroyRequested) {
        android_poll_source* source = nullptr;
        int timeout = (engine.renderer && engine.scene) ? 0 : -1;
        int result = ALooper_pollOnce(timeout, nullptr, nullptr,
                                      reinterpret_cast<void**>(&source));
        if (result == ALOOPER_POLL_ERROR) {
            LOGE("ALooper_pollOnce returned an error");
            break;
        }
        if (source != nullptr) {
            source->process(app, source);
        }

        if (engine.renderer && engine.scene) {
            pumpInput(app, engine.scene.get());

            // Реальная дельта времени кадра.
            auto now = std::chrono::steady_clock::now();
            float dt = 0.0f;
            if (engine.haveTime) {
                dt = std::chrono::duration<float>(now - engine.lastTime).count();
            }
            engine.lastTime = now;
            engine.haveTime = true;
            if (dt > 0.25f) dt = 0.25f;  // защита от «спирали смерти» после паузы

            // Фиксированный тик симуляции: сколько накопилось — столько шагов.
            engine.accumulator += dt;
            while (engine.accumulator >= kTick) {
                engine.scene->fixedUpdate(kTick);
                engine.accumulator -= kTick;
            }
            float alpha = engine.accumulator / kTick;  // доля до следующего тика

            // Рендер с интерполяцией между тиками.
            RenderFrame frame = engine.scene->render(alpha, engine.renderer->aspectRatio(), dt);
            frame.deltaTime = dt;

            // Направление света управляется слайдером ImGui (см. UI ниже).
            frame.lightDir = normalize(
                Vec3{std::cos(engine.ui.lightAngle), 1.0f, std::sin(engine.ui.lightAngle)});

            // FPS — забота приложения (тайминг здесь), а не игровой логики.
            if (dt > 0.0f) {
                engine.ui.fps = engine.ui.fps * 0.92f + (1.0f / dt) * 0.08f;
            }
            char hud[32];
            std::snprintf(hud, sizeof(hud), "FPS: %.0f", (double)engine.ui.fps);
            const float s = engine.uiScale;  // HUD тоже масштабируем по DPI
            frame.hud.push_back({hud, 24.0f * s, 24.0f * s, 40.0f * s, {1.0f, 0.85f, 0.2f}});

            // UI строит общий модуль GameUi (тот же на десктопе). Рендер вызовет
            // это между ImGui NewFrame и Render.
            Engine* e = &engine;
            frame.ui = [e]() { GameUi::build(e->ui, *e->scene); };

            engine.renderer->renderFrame(frame);
        }
    }

    engine.scene.reset();
    engine.renderer.reset();
    LOGI("android_main finished");
}
