#include "engine/render/ui/screens/BattleScreen.h"

#include <string>

#include "imgui.h"

#include "engine/core/Input.h"
#include "engine/render/ui/windows/DebugPanel.h"
#include "game/BuildingConfig.h"
#include "game/Scene.h"

namespace BattleScreen {
namespace {

void drawHud(UiShell::Ctx& ctx) {
    ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.72f);
    if (ImGui::Begin("##battleHud", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoNav)) {
        ImGui::Text("Ресурс: %.0f / %.0f", (double)ctx.scene.resourceCurrent(),
                    (double)ctx.scene.resourceCap());
        if (ctx.scene.netConnected()) {
            ImGui::SameLine();
            ImGui::TextDisabled("| %s", ctx.scene.netHost() ? "HOST" : "CLIENT");
        }
        if (ctx.btn("Меню")) {
            UiShell::pushYesNo("Меню", "Вернуться в главное меню?", [](DialogResult r) {
                if (r == DialogResult::Yes) UiShell::setMode(UiMode::MainMenu);
            });
        }
        ImGui::SameLine();
        if (ctx.btn(UiShell::isDebugOpen() ? "Debug#" : "Debug")) UiShell::toggleDebug();
    }
    ImGui::End();
}

void drawBuild(UiShell::Ctx& ctx) {
    if (!ctx.scene.netConnected()) return;

    ImGui::SetNextWindowPos(ImVec2(16, 90), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(360, 280), ImGuiCond_FirstUseEver);
    if (!ctx.beginPanel("Строительство###uiBuildPanel")) {
        ctx.endPanel();
        return;
    }

    if (!ctx.scene.buildMode()) {
        const EntityType kBuildable[] = {EntityType::Generator, EntityType::Storage,
                                         EntityType::Tower};
        for (EntityType bt : kBuildable) {
            const BuildingInfo* bi = ctx.scene.buildInfo((int)bt);
            if (bi == nullptr || bi->cost <= 0.0f) continue;
            bool afford = ctx.scene.resourceCurrent() >= bi->cost;
            ImGui::BeginDisabled(!afford);
            std::string lbl = (bi->name.empty() ? std::string("Здание") : bi->name) + " (" +
                              std::to_string((int)bi->cost) + ")";
            if (ctx.btn(lbl.c_str())) ctx.scene.beginBuild((int)bt);
            ImGui::EndDisabled();
        }
    } else {
        const BuildingInfo* bi = ctx.scene.buildInfo(ctx.scene.buildType());
        ImGui::Text("Ставим: %s",
                    (bi != nullptr && !bi->name.empty()) ? bi->name.c_str() : "Здание");
        bool valid = ctx.scene.buildGhostValid();
        ImGui::TextColored(valid ? ImVec4(0.4f, 0.9f, 0.4f, 1.0f) : ImVec4(0.95f, 0.5f, 0.4f, 1.0f),
                           valid ? "Клетка свободна — ставь" : "Занято / далеко / нет ресурса");
        if (ctx.btn("Поставить")) ctx.scene.confirmBuild();
        ImGui::SameLine();
        if (ctx.btn("Отмена")) ctx.scene.cancelBuild();
    }

    if (ctx.scene.netConnected()) {
        ImGui::Separator();
        if (ctx.btn("Disconnect")) {
            Scene* scene = &ctx.scene;
            UiShell::pushYesNo("Сеть", "Покинуть сессию?", [scene](DialogResult r) {
                if (r == DialogResult::Yes) {
                    scene->leaveGame();
                    UiShell::setMode(UiMode::MainMenu);
                }
            });
        }
    }
    ctx.endPanel();
}

void drawMatchBanner(Scene& scene) {
    int phase = scene.matchPhase();
    if (phase == 0) return;
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

void drawJoysticks(Scene& scene) {
    auto drawStick = [](const VirtualJoystick& js, ImU32 ring, ImU32 knob) {
        if (!js.active) return;
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        dl->AddCircle(ImVec2(js.ox, js.oy), js.radius, ring, 48, 4.0f);
        dl->AddCircleFilled(ImVec2(js.cx, js.cy), js.radius * 0.4f, knob);
    };
    drawStick(scene.joystick(), IM_COL32(255, 255, 255, 110), IM_COL32(255, 255, 255, 190));
    drawStick(scene.cameraJoystick(), IM_COL32(120, 210, 255, 120),
              IM_COL32(150, 220, 255, 200));
}

}  // namespace

void draw(UiShell::Ctx& ctx) {
    drawHud(ctx);
    drawBuild(ctx);
    BuildingInfoWindow::draw(ctx);
    DebugPanel::draw(ctx);
    drawMatchBanner(ctx.scene);
    drawJoysticks(ctx.scene);
}

}  // namespace BattleScreen
