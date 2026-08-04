#pragma once

#include "engine/render/ui/UiShell.h"

namespace HomePanel {
void draw(UiShell::Ctx& ctx);
}

namespace MenuStubPanels {
void drawInventory(UiShell::Ctx& ctx);
void drawQuests(UiShell::Ctx& ctx);
void drawShop(UiShell::Ctx& ctx);
void drawEvents(UiShell::Ctx& ctx);
}
