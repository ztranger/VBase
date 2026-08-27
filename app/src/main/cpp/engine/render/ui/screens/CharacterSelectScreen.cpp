#include "engine/render/ui/screens/CharacterSelectScreen.h"

#include <cstdio>

#include "imgui.h"

#include "game/Scene.h"

namespace CharacterSelectScreen {

void draw(UiShell::Ctx& ctx) {
    // Панель слева; 3D-превью выбранного персонажа рисует Scene по центру (за панелью).
    ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(340, 520), ImGuiCond_FirstUseEver);

    if (!ctx.beginPanel("Выбор персонажа###uiCharSelect")) {
        ctx.endPanel();
        return;
    }

    ImGui::TextWrapped("Выбери героя и войди в бой.");
    ImGui::Dummy(ImVec2(0, 8));

    const int sel = ctx.scene.selectedCharacter();
    const int n = ctx.scene.rosterCount();
    for (int i = 0; i < n; ++i) {
        const bool isSel = (i == sel);
        if (isSel) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.83f, 0.35f, 1.0f));
        char label[128];
        std::snprintf(label, sizeof(label), "%s %s", isSel ? "\xE2\x96\xB6" : "   ",
                      ctx.scene.rosterName(i));
        if (ctx.btn(label, ImVec2(-1, 0))) {
            ctx.scene.selectCharacter(i);
            ctx.state.charIndex = i;  // платформа сохранит выбор в файл (персист между запусками)
        }
        if (isSel) ImGui::PopStyleColor();
    }

    ImGui::Dummy(ImVec2(0, 12));
    if (ctx.btn("В бой", ImVec2(-1, 0))) UiShell::setMode(UiMode::Battle);
    ImGui::Dummy(ImVec2(0, 4));
    if (ctx.btn("Назад", ImVec2(-1, 0))) UiShell::setMode(UiMode::MainMenu);

    ctx.endPanel();
}

}  // namespace CharacterSelectScreen
