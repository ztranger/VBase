#include <game-activity/native_app_glue/android_native_app_glue.h>
#include <android/asset_manager.h>
#include <android/configuration.h>
#include <android/input.h>
#include <android/native_window.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "imgui.h"

#include "engine/assets/AssetSource.h"
#include "engine/render/GameUi.h"
#include "engine/render/GlRenderer.h"
#include "engine/core/Log.h"
#include "engine/core/RenderFrame.h"
#include "game/Scene.h"
#include "engine/render/VulkanProbe.h"
#include "engine/render/VulkanRenderer.h"

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

namespace {

// Одно тач-событие пальца за кадр (kind: 0 = down, 1 = move, 2 = up).
struct TouchEvent { int id; float x, y; int kind; };

struct Engine {
    std::unique_ptr<Renderer> renderer;
    std::unique_ptr<Scene> scene;
    bool vulkanSupported = false;
    int backend = 0;         // текущий бэкенд: 0 = GL ES3, 1 = Vulkan
    std::chrono::steady_clock::time_point lastTime{};
    bool haveTime = false;
    float accumulator = 0.0f; // накопитель времени для фиксированного тика
    float uiScale = 1.0f;    // масштаб UI по плотности экрана
    GameUiState ui;          // состояние панели — общее с десктопом

    // Мультитач кадра: pumpInput собирает события ВСЕХ пальцев, а в геймплей (twin-stick)
    // диспатчим ПОСЛЕ renderFrame — тогда WantCaptureMouse свеж (тап по GUI не стартует стик).
    // ImGui кормим «первичным» пальцем (pointers[0]) для dev-панели; стики — по id/половине.
    bool touchHad = false;
    float touchX = 0.0f, touchY = 0.0f;  // первичный палец (позиция для ImGui)
    bool touchPressed = false;           // есть ли палец на экране (кнопка ImGui)
    static constexpr int kMaxTouchEvents = 32;
    TouchEvent touchEvents[kMaxTouchEvents];
    int touchEventCount = 0;
};

// Частота симуляции — единый шаг из engine/net/Net.h. Рендер быстрее и интерполирует.
constexpr float kTick = kTickDt;

// Создать рендер по engine->backend (0 = GL, 1 = Vulkan) из окна и построить мир.
// Оба бэкенда создают surface из одного ANativeWindow, поэтому переключение —
// это reset + повторный createRenderer.
bool createRenderer(Engine* engine, android_app* app) {
    // Источник ассетов нужен и рендеру (шейдеры), и сцене (модели/текстуры).
    AndroidAssetSource assets(app->activity->assetManager);
    if (engine->backend == 1) {
        auto r = std::make_unique<VulkanRenderer>();
        if (!r->init(app->window, assets)) { LOGE("VulkanRenderer init failed"); return false; }
        engine->renderer = std::move(r);
    } else {
        auto r = std::make_unique<GlRenderer>();  // glGetProc не нужен: GLES слинкованы на Android
        if (!r->init(app->window, assets)) { LOGE("GlRenderer init failed"); return false; }
        engine->renderer = std::move(r);
    }
    // Мир строится после init рендера: нужны живой GPU-контекст и AAssetManager.
    engine->scene = std::make_unique<Scene>();
    engine->scene->build(*engine->renderer, assets);
    engine->haveTime = false;

    // Масштаб UI по плотности экрана (иначе на HiDPI интерфейс крошечный).
    int density = AConfiguration_getDensity(app->config);
    float scale = 2.5f;
    if (density > 0 && density <= 1000) scale = (float)density / 160.0f;
    if (scale < 1.0f) scale = 1.0f;
    if (scale > 4.0f) scale = 4.0f;
    engine->uiScale = scale;
    engine->scene->setUiScale(scale);
    // Контекст ImGui создаёт рендер в init — стиль масштабируем на свежем контексте.
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;

    engine->ui.backend = engine->backend;
    engine->ui.requestBackend = -1;
    LOGI("Renderer: %s, UI scale %.2f", engine->backend == 1 ? "Vulkan" : "GL ES3", (double)scale);
    return true;
}

void handleCmd(android_app* app, int32_t cmd) {
    auto* engine = static_cast<Engine*>(app->userData);

    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            if (app->window != nullptr) {
                // Если рендер уже жив (INIT без парного TERM) — снести ДО пересоздания:
                // createRenderer делает ImGui::CreateContext внутри init, а деструктор старого
                // рендера потом дёрнул бы Shutdown/DestroyContext уже по НОВОМУ контексту (UAF).
                if (engine->renderer) {
                    engine->scene.reset();
                    engine->renderer.reset();
                }
                createRenderer(engine, app);
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

// Касания -> ImGui (сразу) + ЗАПОМИНАЕМ в engine. Диспатч в геймплей — после рендера
// (см. цикл), когда WantCaptureMouse свеж. Рендер к вводу не причастен.
void pumpInput(android_app* app, Engine* engine) {
    android_input_buffer* input = android_app_swap_input_buffers(app);
    if (input == nullptr) {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    for (uint64_t i = 0; i < input->motionEventsCount; ++i) {
        GameActivityMotionEvent& event = input->motionEvents[i];
        const int32_t action = event.action;
        const int actionMasked = action & AMOTION_EVENT_ACTION_MASK;
        const int ptrIndex = (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
                             AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;

        // ImGui кормим «первичным» пальцем (pointers[0]) — позиция для панели/кнопок.
        if (event.pointerCount > 0) {
            engine->touchX = GameActivityPointerAxes_getX(&event.pointers[0]);
            engine->touchY = GameActivityPointerAxes_getY(&event.pointers[0]);
            io.AddMousePosEvent(engine->touchX, engine->touchY);
        }

        auto push = [engine](int id, float x, float y, int kind) {
            if (engine->touchEventCount < Engine::kMaxTouchEvents)
                engine->touchEvents[engine->touchEventCount++] = TouchEvent{id, x, y, kind};
        };

        switch (actionMasked) {
            case AMOTION_EVENT_ACTION_DOWN:
            case AMOTION_EVENT_ACTION_POINTER_DOWN:
                push((int)event.pointers[ptrIndex].id,
                     GameActivityPointerAxes_getX(&event.pointers[ptrIndex]),
                     GameActivityPointerAxes_getY(&event.pointers[ptrIndex]), 0);
                io.AddMouseButtonEvent(0, true);
                engine->touchPressed = true;
                engine->touchHad = true;
                break;
            case AMOTION_EVENT_ACTION_MOVE:
                for (uint32_t p = 0; p < event.pointerCount; ++p)
                    push((int)event.pointers[p].id,
                         GameActivityPointerAxes_getX(&event.pointers[p]),
                         GameActivityPointerAxes_getY(&event.pointers[p]), 1);
                engine->touchHad = true;
                break;
            case AMOTION_EVENT_ACTION_UP:
            case AMOTION_EVENT_ACTION_POINTER_UP:
                push((int)event.pointers[ptrIndex].id, 0.0f, 0.0f, 2);
                if (event.pointerCount <= 1) {  // последний палец ушёл — отпускаем кнопку ImGui
                    io.AddMouseButtonEvent(0, false);
                    engine->touchPressed = false;
                }
                engine->touchHad = true;
                break;
            case AMOTION_EVENT_ACTION_CANCEL:
                for (uint32_t p = 0; p < event.pointerCount; ++p)
                    push((int)event.pointers[p].id, 0.0f, 0.0f, 2);
                io.AddMouseButtonEvent(0, false);
                engine->touchPressed = false;
                engine->touchHad = true;
                break;
            default:
                break;
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
    engine.ui.vulkanAvailable = engine.vulkanSupported;  // кнопка Vulkan в GameUi
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
            pumpInput(app, &engine);

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

            // Свет задаёт сцена (из файла), правится слайдером в GameUi.

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

            // Диспатч касания в геймплей ПОСЛЕ рендера: NewFrame внутри renderFrame уже
            // пересчитал WantCaptureMouse по этому касанию. Тап по GUI (WantCaptureMouse)
            // -> джойстик/пикинг не активируем. Релиз отдаём всегда (чтобы не залипал).
            if (engine.touchHad && engine.scene) {
                bool guiOwns = ImGui::GetIO().WantCaptureMouse;
                const bool play = GameUi::gameplayActive();  // вне боя стик не стартуем
                float w = app->window != nullptr ? (float)ANativeWindow_getWidth(app->window) : 1.0f;
                float h = app->window != nullptr ? (float)ANativeWindow_getHeight(app->window) : 1.0f;
                for (int i = 0; i < engine.touchEventCount; ++i) {
                    const TouchEvent& e = engine.touchEvents[i];
                    // DOWN не стартует стик, если палец на GUI (тап по панели) или мы вне боя
                    // (меню/лоадинг/диалог). MOVE/UP диспатчим всегда — Scene игнорит id, не
                    // владеющий стиком (не залипнет).
                    if (e.kind == 0) {
                        if (!guiOwns && play) engine.scene->onTouchDown(e.id, e.x, e.y, w, h);
                    } else if (e.kind == 1) {
                        engine.scene->onTouchMove(e.id, e.x, e.y);
                    } else {
                        engine.scene->onTouchUp(e.id);
                    }
                }
                engine.touchHad = false;
                engine.touchEventCount = 0;
            }

            // Переключение бэкенда по кнопке в GameUi: сносим рендер+мир и
            // пересоздаём из того же окна (surface поддерживают оба бэкенда).
            if (engine.ui.requestBackend >= 0 && engine.ui.requestBackend != engine.backend &&
                app->window != nullptr) {
                int nb = engine.ui.requestBackend;
                if (nb == 1 && !engine.vulkanSupported) nb = 0;
                engine.scene.reset();
                engine.renderer.reset();
                engine.backend = nb;
                createRenderer(&engine, app);
            }
        }
    }

    engine.scene.reset();
    engine.renderer.reset();
    LOGI("android_main finished");
}
