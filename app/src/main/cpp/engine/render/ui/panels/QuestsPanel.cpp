#include "engine/render/ui/panels/QuestsPanel.h"

#include "engine/render/ui/panels/StubPanel.h"

namespace QuestsPanel {

void draw(UiShell::Ctx& ctx) {
    StubPanel::body(ctx, "Квесты###uiQuestsPanel", "Журнал заданий и наград.");
}

}  // namespace QuestsPanel
