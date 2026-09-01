#include "engine/render/ui/windows/DebugPanel.h"

#include <cfloat>
#include <cmath>

#include "imgui.h"

#include "engine/render/ui/UiShell.h"
#include "game/Scene.h"

namespace DebugPanel {

void draw(UiShell::Ctx& ctx) {
    if (!UiShell::isDebugOpen()) return;

    ImGui::SetNextWindowPos(ImVec2(20, 90), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(520, 640), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(320, 240), ImVec2(FLT_MAX, FLT_MAX));

    bool open = true;
    if (!ctx.beginPanel("Debug###uiDebugPanel", &open)) {
        ctx.endPanel();
        if (!open) UiShell::showDebug(false);
        return;
    }

    ImGui::Text("FPS: %.1f", (double)ctx.state.fps);
    if (ctx.scene.netConnected())
        ImGui::Text("Ping: %d ms", ctx.scene.netPingMs());
    else if (ctx.scene.netConnecting())
        ImGui::TextDisabled("Ping: -- (подключение)");
    else if (ctx.scene.netConnectionLost())
        ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.30f, 1.0f), "Ping: -- (обрыв)");

    ImGui::SeparatorText("Light");
    Vec3 ld = ctx.scene.lightDir();
    float dir[3] = {ld.x, ld.y, ld.z};
    if (ImGui::SliderFloat3("Light dir", dir, -1.0f, 1.0f)) {
        ctx.scene.setLightDir(Vec3{dir[0], dir[1], dir[2]});
    }

    ImGui::SeparatorText("Shadows");
    bool sh = ctx.scene.shadowsEnabled();
    if (ImGui::Checkbox("Enabled", &sh)) ctx.scene.setShadowsEnabled(sh);
    ImGui::BeginDisabled(!sh);
    float sb = ctx.scene.shadowBias();
    if (ImGui::SliderFloat("Bias", &sb, 0.0002f, 0.010f, "%.4f")) ctx.scene.setShadowBias(sb);
    float sr = ctx.scene.shadowRadius();
    if (ImGui::SliderFloat("Area", &sr, 5.0f, 40.0f, "%.1f")) ctx.scene.setShadowRadius(sr);
    ImGui::EndDisabled();

    ImGui::SeparatorText("Fog");
    // Плотность 0 = туман выключен. Цвет правим в гамма-пространстве (как на экране),
    // храним/шлём в линейном — конверсия тут.
    float fd = ctx.scene.fogDensity();
    if (ImGui::SliderFloat("Density", &fd, 0.0f, 0.06f, "%.3f")) ctx.scene.setFogDensity(fd);
    Vec3 fc = ctx.scene.fogColor();  // linear
    float g[3] = {std::pow(fc.x, 1.0f / 2.2f), std::pow(fc.y, 1.0f / 2.2f), std::pow(fc.z, 1.0f / 2.2f)};
    if (ImGui::ColorEdit3("Color", g)) {
        ctx.scene.setFogColor(Vec3{std::pow(g[0], 2.2f), std::pow(g[1], 2.2f), std::pow(g[2], 2.2f)});
    }

    ImGui::SeparatorText("Renderer");
    bool isGl = (ctx.state.backend == 0);
    ImGui::BeginDisabled(isGl);
    if (ctx.btn("OpenGL")) ctx.state.requestBackend = 0;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!isGl || !ctx.state.vulkanAvailable);
    if (ctx.btn("Vulkan")) ctx.state.requestBackend = 1;
    ImGui::EndDisabled();
    if (!ctx.state.vulkanAvailable) {
        ImGui::SameLine();
        ImGui::TextDisabled("(недоступен)");
    }

    ImGui::SeparatorText("UI");
    ImGui::Text("Mode: %s", UiShell::mode() == UiMode::Battle       ? "Battle"
                            : UiShell::mode() == UiMode::MainMenu ? "MainMenu"
                                                                   : "Loading");
    if (ctx.btn(UiShell::hasLoadingArt() ? "Показать лоадинг" : "Лоадинг (нет арта)")) {
        if (UiShell::hasLoadingArt()) UiShell::setMode(UiMode::Loading);
    }
    if (UiShell::mode() == UiMode::Battle) {
        if (ctx.btn("В главное меню")) {
            UiShell::pushYesNo("Меню", "Вернуться в главное меню?", [](DialogResult r) {
                if (r == DialogResult::Yes) UiShell::setMode(UiMode::MainMenu);
            });
        }
    } else if (UiShell::mode() == UiMode::MainMenu) {
        if (ctx.btn("В бой")) UiShell::setMode(UiMode::Battle);
    }

    ImGui::SeparatorText("Character");
    ImGui::Text("Speed: %.2f", (double)ctx.scene.characterSpeed());
    if (ctx.btn("Jump (Space)")) ctx.scene.requestJump();
    if (ctx.btn("Attack (F)")) ctx.scene.requestAttack();
    float yawOff = ctx.scene.modelYawOffset();
    if (ImGui::SliderFloat("Model yaw", &yawOff, -3.15f, 3.15f)) {
        ctx.scene.setModelYawOffset(yawOff);
    }
    float mscale = ctx.scene.modelScale();
    if (ImGui::SliderFloat("Model scale", &mscale, 0.005f, 0.1f, "%.3f")) {
        ctx.scene.setModelScale(mscale);
    }

    ImGui::SeparatorText("Camera");
    float cd = ctx.scene.cameraDistance();
    if (ImGui::SliderFloat("Distance", &cd, 6.0f, 34.0f)) ctx.scene.setCameraDistance(cd);
    float cp = ctx.scene.cameraPitch();
    if (ImGui::SliderFloat("Pitch", &cp, 0.40f, 1.45f)) ctx.scene.setCameraPitch(cp);

    ctx.endPanel();
    if (!open) UiShell::showDebug(false);
}

}  // namespace DebugPanel
