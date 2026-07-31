#include "engine/render/GameUi.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

#include "imgui.h"

#include "engine/assets/AssetSource.h"
#include "engine/assets/Assets.h"
#include "engine/core/Input.h"  // VirtualJoystick (для оверлея джойстика)
#include "engine/core/Log.h"
#include "engine/core/Renderer.h"
#include "engine/core/Texture.h"
#include "engine/render/UiSkin.h"
#include "game/Scene.h"

namespace GameUi {
namespace {

UiSkin::Assets g_skin;

// Текстуры превью лоадинга (живут вместе со skin / контекстом ImGui).
ImTextureID g_loadingBg = ImTextureID_Invalid;
ImTextureID g_softBlob = ImTextureID_Invalid;
TextureHandle g_loadingBgHandle = 0;
TextureHandle g_softBlobHandle = 0;
float g_loadingAspect = 16.0f / 9.0f;

bool Btn(const char* label, const ImVec2& size = ImVec2(0, 0)) {
    return UiSkin::Button(label, g_skin, size);
}

TextureData makeSoftBlob(uint32_t size) {
    TextureData t;
    t.width = size;
    t.height = size;
    t.rgba.assign((size_t)size * size * 4, 0);
    const float c = (size - 1) * 0.5f;
    const float invR = 1.0f / (c + 0.5f);
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const float dx = ((float)x - c) * invR;
            const float dy = ((float)y - c) * invR;
            float d = std::sqrt(dx * dx + dy * dy);
            if (d > 1.0f) continue;
            // Мягкое ядро + длинный хвост — для «свечения» поверх арта.
            float a = std::clamp(1.0f - d, 0.0f, 1.0f);
            a = a * a * (0.35f + 0.65f * a);
            const size_t i = ((size_t)y * size + x) * 4;
            t.rgba[i + 0] = 255;
            t.rgba[i + 1] = 255;
            t.rgba[i + 2] = 255;
            t.rgba[i + 3] = (uint8_t)std::clamp(a * 255.0f, 0.0f, 255.0f);
        }
    }
    return t;
}

void unloadLoading(Renderer& renderer) {
    auto release = [&](ImTextureID& id, TextureHandle& h) {
        if (id != ImTextureID_Invalid) {
            renderer.releaseImGuiTexture((uint64_t)id);
            id = ImTextureID_Invalid;
        }
        h = 0;
    };
    release(g_loadingBg, g_loadingBgHandle);
    release(g_softBlob, g_softBlobHandle);
}

void loadLoading(Renderer& renderer, AssetSource& assets) {
    unloadLoading(renderer);

    TextureData bg;
    if (loadImageAsset(assets, "ui/loading_core.png", bg)) {
        g_loadingBgHandle = renderer.createTexture(bg, true);
        g_loadingBg = (ImTextureID)renderer.getImGuiTexture(g_loadingBgHandle);
        if (bg.height > 0) g_loadingAspect = (float)bg.width / (float)bg.height;
        LOGI("Loading preview: ui/loading_core.png (%ux%u)", bg.width, bg.height);
    } else {
        LOGW("Loading preview: ui/loading_core.png не найден");
    }

    TextureData blob = makeSoftBlob(128);
    g_softBlobHandle = renderer.createTexture(blob, true);
    g_softBlob = (ImTextureID)renderer.getImGuiTexture(g_softBlobHandle);
}

// Cover: картинка заполняет экран с обрезкой, сохраняя aspect.
void coverRect(ImVec2 display, float imgAspect, ImVec2& outMin, ImVec2& outMax) {
    const float dispAspect = display.x / std::max(display.y, 1.0f);
    ImVec2 size, off;
    if (dispAspect > imgAspect) {
        size.x = display.x;
        size.y = display.x / imgAspect;
        off.x = 0.0f;
        off.y = (display.y - size.y) * 0.5f;
    } else {
        size.y = display.y;
        size.x = display.y * imgAspect;
        off.y = 0.0f;
        off.x = (display.x - size.x) * 0.5f;
    }
    outMin = off;
    outMax = ImVec2(off.x + size.x, off.y + size.y);
}

void addTintedBlob(ImDrawList* dl, ImVec2 center, float radius, ImU32 col) {
    if (radius <= 1.0f) return;
    if (g_softBlob != ImTextureID_Invalid) {
        dl->AddImage(ImTextureRef(g_softBlob),
                     ImVec2(center.x - radius, center.y - radius),
                     ImVec2(center.x + radius, center.y + radius), ImVec2(0, 0), ImVec2(1, 1),
                     col);
    } else {
        // Fallback, если blob-текстура не поднялась.
        dl->AddCircleFilled(center, radius, col, 48);
    }
}

void drawLoadingPreview() {
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 disp = io.DisplaySize;
    if (disp.x < 1.0f || disp.y < 1.0f) return;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    // Без клипа окна — эффекты на весь экран.
    dl->PushClipRect(ImVec2(0, 0), disp, false);

    const float t = (float)ImGui::GetTime();
    const float pulse = 0.55f + 0.45f * std::sin(t * 2.0f);
    const float pulseFast = 0.55f + 0.45f * std::sin(t * 3.4f);

    ImVec2 imgMin, imgMax;
    coverRect(disp, g_loadingAspect, imgMin, imgMax);
    const float imgW = imgMax.x - imgMin.x;
    const float imgH = imgMax.y - imgMin.y;

    // 1) Базовый арт.
    if (g_loadingBg != ImTextureID_Invalid) {
        dl->AddImage(ImTextureRef(g_loadingBg), imgMin, imgMax);
    } else {
        dl->AddRectFilled(ImVec2(0, 0), disp, IM_COL32(26, 20, 16, 255));
    }

    // Лёгкая виньетка ПОД светом (только края), чтобы не гасить ядро.
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(disp.x, disp.y * 0.22f),
                                IM_COL32(0, 0, 0, 160), IM_COL32(0, 0, 0, 160),
                                IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));
    dl->AddRectFilledMultiColor(ImVec2(0, disp.y * 0.72f), disp, IM_COL32(0, 0, 0, 0),
                                IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 200),
                                IM_COL32(0, 0, 0, 200));

    // Ядро на арта ~центр ниши (подстроено под loading_core.png).
    const ImVec2 core(imgMin.x + imgW * 0.50f, imgMin.y + imgH * 0.48f);
    const float baseR = std::min(imgW, imgH);

    // 2) Сильное пульсирующее свечение ядра (несколько непрозрачных слоёв).
    addTintedBlob(dl, core, baseR * 0.70f * (0.90f + 0.10f * pulse),
                  IM_COL32(255, 100, 20, (int)(40 + 35 * pulse)));
    addTintedBlob(dl, core, baseR * 0.42f * (0.92f + 0.08f * pulse),
                  IM_COL32(255, 150, 40, (int)(70 + 55 * pulse)));
    addTintedBlob(dl, core, baseR * 0.22f * (0.94f + 0.06f * pulse),
                  IM_COL32(255, 200, 80, (int)(100 + 70 * pulse)));
    addTintedBlob(dl, core, baseR * 0.09f * (0.96f + 0.04f * pulseFast),
                  IM_COL32(255, 245, 200, (int)(140 + 80 * pulseFast)));
    // Яркое «ядро» кругами — гарантированно видно даже без blob-текстуры.
    dl->AddCircleFilled(core, baseR * 0.035f * (0.95f + 0.05f * pulseFast),
                        IM_COL32(255, 250, 220, (int)(180 + 60 * pulseFast)), 24);

    // 3) Луч: широкий конус вниз (как god-ray на артах) + мерцание.
    const ImVec2 beamEnd(core.x + imgW * 0.02f, imgMax.y - imgH * 0.02f);
    const ImVec2 beamDir(beamEnd.x - core.x, beamEnd.y - core.y);
    for (int i = 0; i < 14; ++i) {
        const float u = (i + 1) / 14.0f;
        const ImVec2 p(core.x + beamDir.x * u, core.y + beamDir.y * u);
        const float widen = baseR * (0.10f + u * 0.38f);
        const float flicker = 0.65f + 0.35f * std::sin(t * 2.8f + u * 8.0f);
        const int alpha = (int)(90.0f * (1.0f - u * 0.55f) * pulse * flicker);
        addTintedBlob(dl, p, widen, IM_COL32(255, 150, 45, alpha));
    }
    // Центральная «жила» луча поярче.
    for (int i = 0; i < 8; ++i) {
        const float u = (i + 1) / 8.0f;
        const ImVec2 p(core.x + beamDir.x * u * 0.85f, core.y + beamDir.y * u * 0.85f);
        const float flicker = 0.7f + 0.3f * std::sin(t * 4.0f + u * 10.0f);
        addTintedBlob(dl, p, baseR * (0.04f + u * 0.10f),
                      IM_COL32(255, 220, 140, (int)(110 * (1.0f - u * 0.4f) * flicker * pulse)));
    }

    // 4) Пыль / искры — крупнее и ярче.
    for (int i = 0; i < 48; ++i) {
        const float seed = (float)i * 12.9898f;
        const float along = std::fmod(t * (0.10f + 0.05f * std::fmod(seed, 1.0f)) +
                                          std::fmod(seed * 0.17f, 1.0f),
                                      1.0f);
        const float side = std::sin(seed * 1.7f + t * 0.9f) * (baseR * (0.02f + along * 0.12f));
        const ImVec2 p(core.x + beamDir.x * along + side,
                       core.y + beamDir.y * along + side * 0.2f);
        const float twinkle = 0.35f + 0.65f * (0.5f + 0.5f * std::sin(t * 6.0f + seed));
        const int a = (int)(220.0f * twinkle * (1.0f - along * 0.35f));
        const float r = 1.8f + along * 3.5f + 1.5f * twinkle;
        dl->AddCircleFilled(p, r, IM_COL32(255, 230, 170, a), 10);
        if ((i % 4) == 0) {
            dl->AddCircleFilled(p, r * 2.2f, IM_COL32(255, 160, 50, a / 3), 10);
        }
    }

    // 5) Подпись / бар внизу (поверх виньетки).
    const char* title = "VBase";
    const float titleFs = ImGui::GetFontSize() * 2.4f;
    ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(titleFs, FLT_MAX, 0.0f, title);
    ImVec2 titlePos(disp.x * 0.5f - ts.x * 0.5f, disp.y * 0.78f);
    dl->AddText(ImGui::GetFont(), titleFs, ImVec2(titlePos.x + 2, titlePos.y + 2),
                IM_COL32(0, 0, 0, 180), title);
    dl->AddText(ImGui::GetFont(), titleFs, titlePos, IM_COL32(242, 230, 212, 255), title);

    const char* tip = "Готовим оборону…";
    ImVec2 tipSz = ImGui::CalcTextSize(tip);
    dl->AddText(ImVec2(disp.x * 0.5f - tipSz.x * 0.5f, titlePos.y + titleFs + 8.0f),
                IM_COL32(184, 169, 148, 255), tip);

    const float barW = std::min(disp.x * 0.45f, 420.0f);
    const float barH = 10.0f;
    const ImVec2 barMin(disp.x * 0.5f - barW * 0.5f, titlePos.y + titleFs + 36.0f);
    const ImVec2 barMax(barMin.x + barW, barMin.y + barH);
    dl->AddRectFilled(barMin, barMax, IM_COL32(42, 34, 28, 230), 4.0f);
    dl->AddRect(barMin, barMax, IM_COL32(196, 165, 116, 200), 4.0f, 0, 1.5f);
    float prog = 0.5f + 0.5f * std::sin(t * 0.9f);
    prog = std::clamp(prog, 0.08f, 1.0f);
    dl->AddRectFilled(barMin, ImVec2(barMin.x + barW * prog, barMax.y),
                      IM_COL32(232, 161, 58, 255), 4.0f);

    dl->PopClipRect();
}

}  // namespace

void loadFont(AssetSource& assets) {
    ImGuiIO& io = ImGui::GetIO();
    // Байты TTF должны жить, пока жив атлас (FontDataOwnedByAtlas=false). Держим их
    // в static — переживают пересоздание контекста при переключении бэкенда.
    static std::vector<uint8_t> data;
    if (data.empty()) assets.read("fonts/ui.ttf", data);
    if (data.empty()) {
        LOGW("UI-шрифт fonts/ui.ttf не найден — кириллица не отрисуется");
        return;  // нет файла -> останется дефолтный шрифт (латиница)
    }
    LOGI("UI-шрифт загружен (%d байт, кириллица)", (int)data.size());

    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    io.Fonts->Clear();
    // GetGlyphRangesCyrillic = базовая латиница + кириллица (английский тоже рисуется).
    io.Fonts->AddFontFromMemoryTTF(data.data(), (int)data.size(), 18.0f, &cfg,
                                   io.Fonts->GetGlyphRangesCyrillic());
}

void loadSkin(Renderer& renderer, AssetSource& assets) {
    if (!UiSkin::load(renderer, assets, g_skin)) {
        LOGW("GameUi: skin не загружен — стандартный вид ImGui");
    }
    loadLoading(renderer, assets);
    // Ресайз окон: с краёв + крупнее hit-zone под палец (Android).
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigWindowsResizeFromEdges = true;
    ImGuiStyle& style = ImGui::GetStyle();
    style.TouchExtraPadding = ImVec2(16.0f, 16.0f);
    style.WindowMinSize = ImVec2(280.0f, 180.0f);
}

void unloadSkin(Renderer& renderer) {
    unloadLoading(renderer);
    UiSkin::unload(renderer, g_skin);
}

void build(GameUiState& state, Scene& scene) {
    // Превью лоадинга поверх всего; управление — маленькое окно сверху.
    if (state.showLoadingPreview) {
        drawLoadingPreview();
        ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.65f);
        if (ImGui::Begin("##loadingPreviewCtrl", nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::TextUnformatted("Превью лоадинга");
            if (Btn("Скрыть")) state.showLoadingPreview = false;
            ImGui::SameLine();
            ImGui::TextDisabled("(или --loading)");
        }
        ImGui::End();
        return;
    }

    // Шире по умолчанию — на телефоне иначе текст в узкой колонке; размер дальше
    // меняется жестом/мышью (ресайз не отключаем).
    ImGui::SetNextWindowPos(ImVec2(20, 90), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(520, 720), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(320, 240), ImVec2(FLT_MAX, FLT_MAX));
    if (g_skin.ready) {
        UiSkin::BeginPanel("VBase", g_skin);
    } else {
        ImGui::Begin("VBase");
    }
    ImGui::Text("FPS: %.1f", (double)state.fps);

    ImGui::SeparatorText("Light");
    Vec3 ld = scene.lightDir();  // направление света сцены (из файла), правим вживую
    float dir[3] = {ld.x, ld.y, ld.z};
    if (ImGui::SliderFloat3("Light dir", dir, -1.0f, 1.0f)) {
        scene.setLightDir(Vec3{dir[0], dir[1], dir[2]});
    }

    ImGui::SeparatorText("Renderer");
    // Кнопка текущего бэкенда неактивна; Vulkan неактивен, если недоступен.
    bool isGl = (state.backend == 0);
    ImGui::BeginDisabled(isGl);
    if (Btn("OpenGL")) state.requestBackend = 0;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!isGl || !state.vulkanAvailable);
    if (Btn("Vulkan")) state.requestBackend = 1;
    ImGui::EndDisabled();
    if (!state.vulkanAvailable) {
        ImGui::SameLine();
        ImGui::TextDisabled("(недоступен)");
    }

    ImGui::SeparatorText("Debug");
    if (Btn(g_loadingBg != ImTextureID_Invalid ? "Показать лоадинг" : "Лоадинг (нет арта)")) {
        if (g_loadingBg != ImTextureID_Invalid) state.showLoadingPreview = true;
    }

    ImGui::SeparatorText("Base");
    ImGui::Text("Resource: %.0f / %.0f", (double)scene.resourceCurrent(),
                (double)scene.resourceCap());

    ImGui::SeparatorText("Character");
    ImGui::Text("Speed: %.2f", (double)scene.characterSpeed());
    if (Btn("Jump (Space)")) scene.requestJump();  // прыжок: кнопка (тач) или пробел
    float yawOff = scene.modelYawOffset();
    if (ImGui::SliderFloat("Model yaw", &yawOff, -3.15f, 3.15f)) {
        scene.setModelYawOffset(yawOff);  // подгонка "морда по движению"
    }
    float mscale = scene.modelScale();
    if (ImGui::SliderFloat("Model scale", &mscale, 0.005f, 0.1f, "%.3f")) {
        scene.setModelScale(mscale);
    }

    ImGui::SeparatorText("Camera");
    float cd = scene.cameraDistance();
    if (ImGui::SliderFloat("Distance", &cd, 6.0f, 34.0f)) scene.setCameraDistance(cd);
    float cp = scene.cameraPitch();
    if (ImGui::SliderFloat("Pitch", &cp, 0.40f, 1.45f)) scene.setCameraPitch(cp);  // ¾-наклон, рад

    ImGui::SeparatorText("Network");
    if (scene.netConnected()) {
        ImGui::Text("%s | remotes: %d", scene.netHost() ? "HOST" : "CLIENT",
                    scene.remoteCount());
        if (Btn("Disconnect")) scene.leaveGame();
    } else {
        // Поле ввода IP: на десктопе работает системная клавиатура. На Android
        // системная софт-клавиатура из ImGui не поднимается, поэтому ниже —
        // своя цифровая клавиатура (тапы работают), она пишет в тот же буфер.
        ImGui::InputText("IP", state.joinIp, sizeof(state.joinIp),
                         ImGuiInputTextFlags_CharsDecimal);

        auto key = [&state](char c) {
            size_t l = std::strlen(state.joinIp);
            if (l + 1 < sizeof(state.joinIp)) {
                state.joinIp[l] = c;
                state.joinIp[l + 1] = '\0';
            }
        };
        float b = ImGui::GetFontSize() * 2.2f;
        ImVec2 sz(b, b);
        const char* rows[3] = {"789", "456", "123"};
        for (int r = 0; r < 3; ++r) {
            for (int i = 0; i < 3; ++i) {
                char lbl[2] = {rows[r][i], '\0'};
                if (i > 0) ImGui::SameLine();
                if (Btn(lbl, sz)) key(rows[r][i]);
            }
        }
        if (Btn("0", sz)) key('0');
        ImGui::SameLine();
        if (Btn(".", sz)) key('.');
        ImGui::SameLine();
        if (Btn("<-", sz)) {
            size_t l = std::strlen(state.joinIp);
            if (l > 0) state.joinIp[l - 1] = '\0';
        }
        ImGui::SameLine();
        if (Btn("Clr", sz)) state.joinIp[0] = '\0';

        if (Btn("Host")) scene.hostGame();
        ImGui::SameLine();
        if (Btn("Join")) scene.joinGame(state.joinIp);
    }
    if (g_skin.ready) UiSkin::EndPanel();
    else ImGui::End();

    // Панель информации о выделенном кликом/тапом здании. Содержимое — из конфига,
    // и разное по типу сущности. Стабильный ID окна (###) — позиция не скачет при
    // смене выделенного типа; видимая подпись — имя из конфига.
    int selType = scene.selectedEntityType();
    const BuildingInfo* info = scene.selectedInfo();
    if (selType >= 0 && info != nullptr) {
        ImGui::SetNextWindowPos(ImVec2(400, 90), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(440, 320), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(280, 180), ImVec2(FLT_MAX, FLT_MAX));
        std::string title = (info->name.empty() ? "Здание" : info->name) + "###buildInfoPanel";
        bool open = true;
        if (g_skin.ready) UiSkin::BeginPanel(title.c_str(), g_skin, &open);
        else ImGui::Begin(title.c_str(), &open);
        if (!info->desc.empty()) ImGui::TextWrapped("%s", info->desc.c_str());
        ImGui::Separator();
        switch ((EntityType)selType) {
            case EntityType::Generator:
                ImGui::Text("Генерация: %.0f / сек", (double)info->rate);
                break;
            case EntityType::Storage:
                ImGui::Text("Ёмкость: %.0f", (double)info->cap);
                ImGui::Text("Сейчас: %.0f", (double)scene.selectedAux());
                break;
            case EntityType::Spawner:
                ImGui::Text("Интервал: %.1f с", (double)info->rate);
                ImGui::Text("Максимум врагов: %.0f", (double)info->cap);
                break;
            case EntityType::Tower:
                ImGui::Text("Урон: %.0f / выстрел", (double)info->damage);
                ImGui::Text("Радиус: %.1f", (double)info->range);
                ImGui::Text("Интервал: %.2f с", (double)info->rate);
                break;
            case EntityType::Core: {
                float ch = scene.coreHp();
                ImGui::Text("Здоровье: %.0f / %.0f", (double)(ch < 0.0f ? 0.0f : ch),
                            (double)info->hp);
                break;
            }
            default:
                break;
        }
        if (Btn("Закрыть")) scene.clearSelection();
        if (g_skin.ready) UiSkin::EndPanel();
        else ImGui::End();
        if (!open) scene.clearSelection();
    }

    // Баннер исхода матча по центру экрана (крупный текст поверх всего).
    int phase = scene.matchPhase();
    if (phase != 0) {  // 1 = победа, 2 = поражение
        const char* msg = (phase == 1) ? "ПОБЕДА" : "ПОРАЖЕНИЕ";
        ImU32 col = (phase == 1) ? IM_COL32(120, 230, 120, 255) : IM_COL32(240, 90, 80, 255);
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        ImGuiIO& io = ImGui::GetIO();
        float scale = 3.0f;
        float fs = ImGui::GetFontSize() * scale;
        ImVec2 ts = ImGui::CalcTextSize(msg);
        ImVec2 pos(io.DisplaySize.x * 0.5f - ts.x * scale * 0.5f,
                   io.DisplaySize.y * 0.32f - fs * 0.5f);
        dl->AddText(ImGui::GetFont(), fs, ImVec2(pos.x + 3, pos.y + 3), IM_COL32(0, 0, 0, 200), msg);
        dl->AddText(ImGui::GetFont(), fs, pos, col, msg);
    }

    // Виртуальные джойстики поверх всего (появляются под пальцами на тач-экране; на десктопе
    // неактивны). Левый — движение героя (белый), правый — камера (голубой).
    auto drawStick = [](const VirtualJoystick& js, ImU32 ring, ImU32 knob) {
        if (!js.active) return;
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        dl->AddCircle(ImVec2(js.ox, js.oy), js.radius, ring, 48, 4.0f);
        dl->AddCircleFilled(ImVec2(js.cx, js.cy), js.radius * 0.4f, knob);
    };
    drawStick(scene.joystick(), IM_COL32(255, 255, 255, 110), IM_COL32(255, 255, 255, 190));
    drawStick(scene.cameraJoystick(), IM_COL32(120, 210, 255, 120), IM_COL32(150, 220, 255, 200));
}

}  // namespace GameUi
