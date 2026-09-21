#include "engine/render/ui/panels/ShopPanel.h"

#include "engine/render/ui/panels/StubPanel.h"

namespace ShopPanel {

void draw(UiShell::Ctx& ctx) {
    StubPanel::body(ctx, "Магазин###uiShopPanel", "Покупка построек и улучшений.");
}

}  // namespace ShopPanel
