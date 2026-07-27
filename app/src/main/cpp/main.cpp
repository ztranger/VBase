#include <game-activity/native_app_glue/android_native_app_glue.h>
#include <android/configuration.h>
#include <android/input.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

#include "imgui.h"

#include "GlRenderer.h"
#include "Log.h"
#include "RenderFrame.h"
#include "Scene.h"
#include "VulkanProbe.h"

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
    float fps = 0.0f;        // сглаженный счётчик кадров для HUD
    float lightAngle = 0.9f; // управляется слайдером ImGui
    bool showDemo = true;    // окно демо ImGui
    float uiScale = 1.0f;    // масштаб UI по плотности экрана
};

void handleCmd(android_app* app, int32_t cmd) {
    auto* engine = static_cast<Engine*>(app->userData);

    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            if (app->window != nullptr) {
                auto renderer = std::make_unique<GlRenderer>();
                if (renderer->init(app->window)) {
                    engine->renderer = std::move(renderer);
                    // Мир строится после инициализации рендера: ему нужны
                    // живой GPU-контекст (залить меши/текстуры) и AAssetManager
                    // (загрузить модели/картинки из APK).
                    engine->scene = std::make_unique<Scene>();
                    engine->scene->build(*engine->renderer, app->activity->assetManager);
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

            // Дельта времени для кадра.
            auto now = std::chrono::steady_clock::now();
            float dt = 0.0f;
            if (engine.haveTime) {
                dt = std::chrono::duration<float>(now - engine.lastTime).count();
            }
            engine.lastTime = now;
            engine.haveTime = true;

            engine.scene->update(dt);
            RenderFrame frame = engine.scene->buildFrame(engine.renderer->aspectRatio());
            frame.deltaTime = dt;

            // Направление света управляется слайдером ImGui (см. UI ниже).
            frame.lightDir = normalize(
                Vec3{std::cos(engine.lightAngle), 1.0f, std::sin(engine.lightAngle)});

            // FPS — забота приложения (тайминг здесь), а не игровой логики.
            if (dt > 0.0f) {
                engine.fps = engine.fps * 0.92f + (1.0f / dt) * 0.08f;
            }
            char hud[32];
            std::snprintf(hud, sizeof(hud), "FPS: %.0f", (double)engine.fps);
            const float s = engine.uiScale;  // HUD тоже масштабируем по DPI
            frame.hud.push_back({hud, 24.0f * s, 24.0f * s, 40.0f * s, {1.0f, 0.85f, 0.2f}});

            // UI строит приложение; рендер вызовет это между ImGui NewFrame и Render.
            Engine* e = &engine;
            frame.ui = [e]() {
                ImGui::SetNextWindowPos(ImVec2(20, 90), ImGuiCond_FirstUseEver);
                ImGui::SetNextWindowSize(ImVec2(340, 260), ImGuiCond_FirstUseEver);
                ImGui::Begin("VBase");
                ImGui::Text("FPS: %.1f", (double)e->fps);
                ImGui::SliderFloat("Light angle", &e->lightAngle, 0.0f, 6.2831f);

                Scene* sc = e->scene.get();
                if (sc != nullptr && sc->animationCount() > 0) {
                    ImGui::SeparatorText("Fox (glTF skinning + blend)");

                    ImGui::TextUnformatted("From:");
                    int a = sc->animA();
                    for (int i = 0; i < sc->animationCount(); ++i) {
                        ImGui::SameLine();
                        if (ImGui::RadioButton((std::string(sc->animationName(i)) + "##A").c_str(), a == i)) {
                            sc->setAnimA(i);
                        }
                    }

                    ImGui::TextUnformatted("To:  ");
                    int b = sc->animB();
                    for (int i = 0; i < sc->animationCount(); ++i) {
                        ImGui::SameLine();
                        if (ImGui::RadioButton((std::string(sc->animationName(i)) + "##B").c_str(), b == i)) {
                            sc->setAnimB(i);
                        }
                    }

                    float blend = sc->blend();
                    if (ImGui::SliderFloat("Blend A<->B", &blend, 0.0f, 1.0f, "%.2f")) {
                        sc->setBlend(blend);
                    }
                    float scale = sc->modelScale();
                    if (ImGui::SliderFloat("Fox scale", &scale, 0.005f, 0.1f, "%.3f")) {
                        sc->setModelScale(scale);
                    }
                }

                ImGui::Checkbox("ImGui demo window", &e->showDemo);
                ImGui::End();
                if (e->showDemo) {
                    ImGui::ShowDemoWindow(&e->showDemo);
                }
            };

            engine.renderer->renderFrame(frame);
        }
    }

    engine.scene.reset();
    engine.renderer.reset();
    LOGI("android_main finished");
}
