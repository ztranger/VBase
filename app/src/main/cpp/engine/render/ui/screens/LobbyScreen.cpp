#include "engine/render/ui/screens/LobbyScreen.h"

#include <algorithm>
#include <cstdio>

#include "imgui.h"

#include "engine/render/ui/UiPalette.h"
#include "game/Scene.h"

namespace LobbyScreen {
namespace {

// Левая колонка: выбор героя + статы выбранного. 3D-превью рисует Scene позади (путь
// CharacterPreview). Внизу колонки — CTA «В бой» / «Назад».
void drawHeroColumn(UiShell::Ctx& ctx) {
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    const float m = UiShell::uiMargin();
    const float font = ImGui::GetFontSize();
    if (!ctx.beginPanelRect("Герой###uiLobbyHero", ImVec2(m, m),
                            ImVec2(font * 16.0f, disp.y - m * 2.0f))) {
        ctx.endPanel();
        return;
    }

    // Выбор героя живёт в главном меню (Home). Здесь — только read-only сводка выбранного перед
    // входом в бой: селектор убран (дублировал Home, а после Host/Join уже заблокирован — тип
    // закоммичен сервером по первому инпуту). Сменить героя — в меню до подключения.
    const int sel = ctx.scene.selectedCharacter();
    const int n = ctx.scene.rosterCount();
    ImGui::TextUnformatted("Герой");
    ImGui::Dummy(ImVec2(0, 6));
    if (sel >= 0 && sel < n) {
        ImGui::PushStyleColor(ImGuiCol_Text, UiPalette::v4(UiPalette::Amber));
        ImGui::TextWrapped("%s", ctx.scene.rosterName(sel));
        ImGui::PopStyleColor();
    } else {
        ImGui::TextDisabled("Герой не выбран — выбери в меню.");
    }

    // Статы выбранного героя (из characters.cfg; клиент бой не считает — только показывает).
    if (sel >= 0 && sel < n) {
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::SeparatorText("Характеристики");
        auto row = [](const char* label, const char* value) {
            ImGui::TextUnformatted(label);
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(value).x);
            ImGui::TextUnformatted(value);
        };
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.0f", (double)ctx.scene.rosterHp(sel));
        row("HP", buf);
        const float rng = ctx.scene.rosterRange(sel);
        if (ctx.scene.rosterDamage(sel) > 0.0f && rng > 0.0f) {
            std::snprintf(buf, sizeof(buf), "%.0f", (double)ctx.scene.rosterDamage(sel));
            row("Урон", buf);
            std::snprintf(buf, sizeof(buf), "%.0f · %s", (double)rng,
                          ctx.scene.rosterRanged(sel) ? "дальн." : "ближн.");
            row("Дальность", buf);
        } else {
            row("Атака", "—");  // не задана в конфиге
        }
        std::snprintf(buf, sizeof(buf), "%.1f", (double)ctx.scene.rosterSpeed(sel));
        row("Скорость", buf);
    }

    // CTA прижимаем к низу колонки.
    const float ctaH = font * 2.2f;
    float y = ImGui::GetWindowHeight() - (ctaH * 2.0f + font * 1.2f);
    if (y > ImGui::GetCursorPosY()) ImGui::SetCursorPosY(y);
    if (ctx.btn("В бой", ImVec2(-1, ctaH))) UiShell::setMode(UiMode::Battle);
    ImGui::Dummy(ImVec2(0, 4));
    if (ctx.btn("Назад", ImVec2(-1, 0))) UiShell::back();

    ctx.endPanel();
}

// Top-down мини-карта сцены из Scene::sceneMinimap() (плоскость XZ): стены-препятствия + точки
// интереса (ядро/спавнеры/старт героя). Рисуем в draw-list с равномерным масштабом (без искажения),
// маркеры проецируем теми же bounds. x -> вправо, z -> вниз (схематично, без компаса).
void drawMinimap(UiShell::Ctx& ctx) {
    const Scene::Minimap mm = ctx.scene.sceneMinimap();
    const float font = ImGui::GetFontSize();
    const float boxW = ImGui::GetContentRegionAvail().x;
    const float boxH = boxW * 0.68f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + boxW, p0.y + boxH);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    dl->AddRectFilled(p0, p1, UiPalette::Void, font * 0.25f);
    dl->AddRect(p0, p1, UiPalette::withAlpha(UiPalette::Amber, 140), font * 0.25f);

    const float wW = mm.maxX - mm.minX, wH = mm.maxZ - mm.minZ;
    if (mm.valid && wW > 1e-3f && wH > 1e-3f) {
        const float pad = font * 0.5f;
        const float scale = std::min((boxW - pad * 2.0f) / wW, (boxH - pad * 2.0f) / wH);
        const float ox = p0.x + (boxW - wW * scale) * 0.5f;  // центрируем карту в боксе
        const float oy = p0.y + (boxH - wH * scale) * 0.5f;
        auto P = [&](float x, float z) {
            return ImVec2(ox + (x - mm.minX) * scale, oy + (z - mm.minZ) * scale);
        };
        dl->PushClipRect(p0, p1, true);
        for (const Scene::Minimap::Wall& w : mm.walls)
            dl->AddRectFilled(P(w.cx - w.hx, w.cz - w.hz), P(w.cx + w.hx, w.cz + w.hz),
                              UiPalette::withAlpha(UiPalette::StoneLight, 220));
        const float r = font * 0.32f;
        for (const Scene::Minimap::Mark& m : mm.marks) {
            ImVec2 c = P(m.x, m.z);
            ImU32 col = UiPalette::Success;   // HeroSpawn
            float rr = r;
            if (m.kind == Scene::Minimap::Poi::Core) { col = UiPalette::Gold; rr = r * 1.5f; }
            else if (m.kind == Scene::Minimap::Poi::Spawner) col = UiPalette::Danger;
            dl->AddCircleFilled(c, rr, col);
            dl->AddCircle(c, rr, UiPalette::withAlpha(UiPalette::Void, 200), 0, 1.5f);
        }
        dl->PopClipRect();
    }
    ImGui::Dummy(ImVec2(boxW, boxH));  // резервируем место под layout

    // Легенда цветов.
    ImGui::PushStyleColor(ImGuiCol_Text, UiPalette::v4(UiPalette::Gold));
    ImGui::TextUnformatted("\xE2\x97\x8F ядро");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, UiPalette::v4(UiPalette::Danger));
    ImGui::TextUnformatted("\xE2\x97\x8F спавн");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, UiPalette::v4(UiPalette::Success));
    ImGui::TextUnformatted("\xE2\x97\x8F старт");
    ImGui::PopStyleColor();
}

// Правая колонка: read-only брифинг уже выбранной сцены (карта выбирается раньше, в меню).
void drawBriefingColumn(UiShell::Ctx& ctx) {
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    const float m = UiShell::uiMargin();
    const float font = ImGui::GetFontSize();
    const float w = font * 19.0f;
    if (!ctx.beginPanelRect("Брифинг###uiLobbyBrief", ImVec2(disp.x - m - w, m),
                            ImVec2(w, disp.y - m * 2.0f))) {
        ctx.endPanel();
        return;
    }

    ImGui::TextUnformatted("Брифинг");

    // Название сцены: из манифеста, иначе путь.
    const char* name = ctx.scene.sceneListName(ctx.scene.currentSceneIndex());
    if (name == nullptr || name[0] == '\0') name = ctx.scene.currentScenePath();
    ImGui::PushStyleColor(ImGuiCol_Text, UiPalette::v4(UiPalette::Amber));
    ImGui::TextWrapped("%s", (name && name[0]) ? name : "(сцена)");
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0, 4));
    ImGui::SeparatorText("Карта");
    drawMinimap(ctx);

    const Scene::SceneBriefing b = ctx.scene.sceneBriefing();

    ImGui::Dummy(ImVec2(0, 4));
    ImGui::SeparatorText("Задача");
    auto row = [](const char* label, const char* value) {
        ImGui::TextUnformatted(label);
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(value).x);
        ImGui::TextUnformatted(value);
    };
    row("Цель", b.pvp ? "уничтожить ядро врага" : (b.hasCore ? "защитить ядро" : "выжить"));
    if (b.hasCore) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.0f", (double)b.coreHp);
        row("HP ядра", buf);
    }
    if (b.infiniteWaves) {
        char buf[48];
        if (b.waveGrow > 0)
            std::snprintf(buf, sizeof(buf), "беск., +%d/волна", b.waveGrow);
        else
            std::snprintf(buf, sizeof(buf), "бесконечные");
        row("Волны", buf);
        char sb[32];
        std::snprintf(sb, sizeof(sb), "%d", b.spawnerCount);
        row("Спавнеров", sb);
    } else if (b.spawnerCount > 0) {
        row("Волны", "фиксированные");
    }
    row("Режим", b.pvp ? "PvP" : "соло / кооп");

    ctx.endPanel();
}

}  // namespace

void draw(UiShell::Ctx& ctx) {
    drawHeroColumn(ctx);
    drawBriefingColumn(ctx);
}

}  // namespace LobbyScreen
