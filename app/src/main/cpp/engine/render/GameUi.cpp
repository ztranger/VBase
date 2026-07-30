#include "engine/render/GameUi.h"

#include <cfloat>
#include <cstring>
#include <string>
#include <vector>

#include "imgui.h"

#include "engine/assets/AssetSource.h"
#include "engine/core/Input.h"  // VirtualJoystick (для оверлея джойстика)
#include "engine/core/Log.h"
#include "game/Scene.h"
#include "engine/render/UiSkin.h"

namespace GameUi {
namespace {

UiSkin::Assets g_skin;

bool Btn(const char* label, const ImVec2& size = ImVec2(0, 0)) {
    return UiSkin::Button(label, g_skin, size);
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
    // Ресайз окон: с краёв + крупнее hit-zone под палец (Android).
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigWindowsResizeFromEdges = true;
    ImGuiStyle& style = ImGui::GetStyle();
    style.TouchExtraPadding = ImVec2(16.0f, 16.0f);
    style.WindowMinSize = ImVec2(280.0f, 180.0f);
}

void unloadSkin(Renderer& renderer) { UiSkin::unload(renderer, g_skin); }

void build(GameUiState& state, Scene& scene) {
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
    if (ImGui::SliderFloat("Distance", &cd, 2.0f, 15.0f)) scene.setCameraDistance(cd);
    float ch = scene.cameraHeight();
    if (ImGui::SliderFloat("Height", &ch, 0.5f, 10.0f)) scene.setCameraHeight(ch);

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
            default:
                break;
        }
        if (Btn("Закрыть")) scene.clearSelection();
        if (g_skin.ready) UiSkin::EndPanel();
        else ImGui::End();
        if (!open) scene.clearSelection();
    }

    // Виртуальный джойстик поверх всего (появляется под пальцем на тач-экране;
    // на десктопе неактивен, поэтому ничего не рисуется).
    if (scene.joystick().active) {
        const VirtualJoystick& js = scene.joystick();
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        dl->AddCircle(ImVec2(js.ox, js.oy), js.radius, IM_COL32(255, 255, 255, 110), 48, 4.0f);
        dl->AddCircleFilled(ImVec2(js.cx, js.cy), js.radius * 0.4f,
                            IM_COL32(255, 255, 255, 190));
    }
}

}  // namespace GameUi
