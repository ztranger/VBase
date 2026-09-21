#include "engine/render/ui/screens/MainMenuScreen.h"

#include "imgui.h"

#include "engine/render/ui/panels/EventsPanel.h"
#include "engine/render/ui/panels/HomePanel.h"
#include "engine/render/ui/panels/InventoryPanel.h"
#include "engine/render/ui/panels/QuestsPanel.h"
#include "engine/render/ui/panels/ShopPanel.h"
#include "engine/render/ui/windows/DebugPanel.h"

namespace MainMenuScreen {
namespace {

void drawChrome(UiShell::Ctx& ctx) {
    const float font = ImGui::GetFontSize();
    const float barH = UiShell::navBarHeight();
    const ImVec2 sz(ImGui::GetIO().DisplaySize.x, barH);  // нижний нав-бар на всю ширину
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(font * 0.45f, font * 0.45f));
    if (ctx.beginOverlay("##mainMenuChrome",
                         UiShell::anchorPos(UiShell::Anchor::BottomLeft, sz, 0.0f), sz, 0.94f,
                         ImGuiWindowFlags_NoScrollbar)) {
        auto nav = [&](const char* label, MainMenuPanel id) {
            // Активная вкладка — через selected скина (обычный PushStyleColor скин игнорит).
            if (ctx.btn(label, ImVec2(0, barH - font), /*selected=*/UiShell::panel() == id))
                UiShell::setPanel(id);
            ImGui::SameLine();
        };
        nav("Главная", MainMenuPanel::Home);
        nav("Инвентарь", MainMenuPanel::Inventory);
        nav("Квесты", MainMenuPanel::Quests);
        nav("Магазин", MainMenuPanel::Shop);
        nav("Ивенты", MainMenuPanel::Events);
    }
    ctx.endOverlay();
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
            InventoryPanel::draw(ctx);
            break;
        case MainMenuPanel::Quests:
            QuestsPanel::draw(ctx);
            break;
        case MainMenuPanel::Shop:
            ShopPanel::draw(ctx);
            break;
        case MainMenuPanel::Events:
            EventsPanel::draw(ctx);
            break;
    }

    DebugPanel::draw(ctx);
}

}  // namespace MainMenuScreen
