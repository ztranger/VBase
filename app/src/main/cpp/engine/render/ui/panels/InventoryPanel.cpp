#include "engine/render/ui/panels/InventoryPanel.h"

#include "engine/render/ui/panels/StubPanel.h"

namespace InventoryPanel {

void draw(UiShell::Ctx& ctx) {
    StubPanel::body(ctx, "Инвентарь###uiInvPanel", "Снаряжение героя и расходники.");
}

}  // namespace InventoryPanel
