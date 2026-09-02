#include "engine/render/ui/screens/BattleScreen.h"

#include <cfloat>
#include <cstdio>
#include <string>

#include "imgui.h"

#include "engine/core/Input.h"
#include "engine/render/ui/windows/BuildingInfo.h"
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

// Читаемость боя: worldspace HP-бары + всплывающие числа урона. Данные производит Scene
// (диф hp из снапшотов), здесь только проецируем мировые точки через view/proj и рисуем.
// Фоновый draw-list — над 3D, но ПОД окнами ImGui (бары не перекрывают HUD/панели).
void drawCombatOverlay(Scene& scene) {
    if (!scene.netConnected()) return;
    const std::vector<Scene::CombatMarker>& markers = scene.combatMarkers();
    const std::vector<Scene::DamageNumber>& numbers = scene.damageNumbers();
    if (markers.empty() && numbers.empty()) return;

    ImGuiIO& io = ImGui::GetIO();
    const float W = io.DisplaySize.x, H = io.DisplaySize.y;
    const Mat4 vp = scene.projMatrix() * scene.viewMatrix();
    const float* m = vp.m;  // column-major: clip = VP * (x,y,z,1)
    auto project = [&](const Vec3& p, float& sx, float& sy) -> bool {
        float cx = m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12];
        float cy = m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13];
        float cw = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
        if (cw <= 0.0001f) return false;  // точка за камерой
        float ndcx = cx / cw, ndcy = cy / cw;
        if (ndcx < -1.3f || ndcx > 1.3f || ndcy < -1.3f || ndcy > 1.3f) return false;  // за краем
        sx = (ndcx * 0.5f + 0.5f) * W;
        sy = (1.0f - (ndcy * 0.5f + 0.5f)) * H;
        return true;
    };

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    for (const Scene::CombatMarker& mk : markers) {
        float sx, sy;
        if (!project(mk.pos, sx, sy)) continue;
        const float bw = 46.0f, bh = 6.0f;
        ImVec2 a(sx - bw * 0.5f, sy - bh * 0.5f), b(sx + bw * 0.5f, sy + bh * 0.5f);
        float f = mk.hpFrac < 0.0f ? 0.0f : (mk.hpFrac > 1.0f ? 1.0f : mk.hpFrac);
        float xmid = a.x + (b.x - a.x) * f;
        dl->AddRectFilled(ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 1), IM_COL32(0, 0, 0, 190), 2.0f);
        dl->AddRectFilled(ImVec2(xmid, a.y), b, IM_COL32(30, 30, 30, 170), 2.0f);  // пустой остаток
        ImU32 col = IM_COL32((int)(mk.color.x * 255), (int)(mk.color.y * 255), (int)(mk.color.z * 255), 235);
        dl->AddRectFilled(a, ImVec2(xmid, b.y), col, 2.0f);  // заполнение по доле hp
    }

    ImFont* font = ImGui::GetFont();
    const float fs = ImGui::GetFontSize() * 1.2f;
    for (const Scene::DamageNumber& dn : numbers) {
        float sx, sy;
        if (!project(dn.pos, sx, sy)) continue;
        float lifeT = dn.age / 0.9f;  // синхронно с kDmgLife в Scene
        if (lifeT > 1.0f) lifeT = 1.0f;
        int alpha = (int)(255.0f * (1.0f - lifeT * lifeT));  // держится, потом резко гаснет
        sy -= dn.age * 42.0f;  // всплывает вверх в экранных пикселях (стабильно на любой дистанции)
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d", (int)(dn.amount + 0.5f));
        ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, buf);
        ImVec2 tp(sx - ts.x * 0.5f, sy - ts.y * 0.5f);
        dl->AddText(font, fs, ImVec2(tp.x + 1.5f, tp.y + 1.5f), IM_COL32(0, 0, 0, alpha), buf);
        dl->AddText(font, fs, tp,
                    IM_COL32((int)(dn.color.x * 255), (int)(dn.color.y * 255), (int)(dn.color.z * 255), alpha),
                    buf);
    }
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
    drawCombatOverlay(ctx.scene);  // HP-бары/числа — под окнами (фоновый draw-list)
    drawHud(ctx);
    drawBuild(ctx);
    BuildingInfoWindow::draw(ctx);
    DebugPanel::draw(ctx);
    drawMatchBanner(ctx.scene);
    drawJoysticks(ctx.scene);
}

}  // namespace BattleScreen
