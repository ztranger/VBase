#pragma once

#include "imgui.h"

#include "engine/render/ui/UiShell.h"

// Общее тело панели-заглушки раздела хаба (Инвентарь/Квесты/Магазин/События). Каждый раздел
// живёт в своём файле (панель = один файл), чтобы большой проход по контенту наполнял их
// независимо, не трогая HomePanel. Пока — единый плейсхолдер; при наполнении раздел просто
// перестаёт звать этот хелпер и рисует своё. См. docs/UI_SYSTEM.md.
namespace StubPanel {

inline void body(UiShell::Ctx& ctx, const char* title, const char* tip) {
    ImVec2 pos, size;
    UiShell::menuContentRect(pos, size);  // заякорить на всю область над нав-баром
    if (!ctx.beginPanelRect(title, pos, size)) {
        ctx.endPanel();
        return;
    }
    ImGui::TextWrapped("%s", tip);
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::TextDisabled("Заглушка — контент появится позже.");
    if (ctx.btn("На главную")) UiShell::setPanel(MainMenuPanel::Home);
    ctx.endPanel();
}

}  // namespace StubPanel
