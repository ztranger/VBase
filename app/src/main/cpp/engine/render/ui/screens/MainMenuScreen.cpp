#include "engine/render/ui/screens/MainMenuScreen.h"

#include "imgui.h"

#include "engine/render/ui/panels/HomePanel.h"
#include "engine/render/ui/windows/DebugPanel.h"

namespace MainMenuScreen {
namespace {

void drawChrome(UiShell::Ctx& ctx) {
    ImGuiIO& io = ImGui::GetIO();
    const float barH = 56.0f;
    ImGui::SetNextWindowPos(ImVec2(0, io.DisplaySize.y - barH), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, barH), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
    ImGui::Begin("##mainMenuChrome", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse);

    auto nav = [&](const char* label, MainMenuPanel id) {
        const bool active = UiShell::panel() == id;
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.72f, 0.45f, 0.14f, 1.0f));
        if (ctx.btn(label, ImVec2(0, barH - 16))) UiShell::setPanel(id);
        if (active) ImGui::PopStyleColor();
        ImGui::SameLine();
    };

    nav("Главная", MainMenuPanel::Home);
    nav("Инвентарь", MainMenuPanel::Inventory);
    nav("Квесты", MainMenuPanel::Quests);
    nav("Магазин", MainMenuPanel::Shop);
    nav("Ивенты", MainMenuPanel::Events);

    ImGui::End();
    ImGui::PopStyleVar();
}

}  // namespace

void draw(UiShell::Ctx& ctx) {
    drawChrome(ctx);

    switch (UiShell::panel()) {
        case MainMenuPanel::Home:
            HomePanel::draw(ctx);
            break;
        case MainMenuPanel::Inventory:
            MenuStubPanels::drawInventory(ctx);
            break;
        case MainMenuPanel::Quests:
            MenuStubPanels::drawQuests(ctx);
            break;
        case MainMenuPanel::Shop:
            MenuStubPanels::drawShop(ctx);
            break;
        case MainMenuPanel::Events:
            MenuStubPanels::drawEvents(ctx);
            break;
    }

    DebugPanel::draw(ctx);
}

}  // namespace MainMenuScreen
