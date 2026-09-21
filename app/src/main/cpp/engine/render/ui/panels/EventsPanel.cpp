#include "engine/render/ui/panels/EventsPanel.h"

#include "engine/render/ui/panels/StubPanel.h"

namespace EventsPanel {

void draw(UiShell::Ctx& ctx) {
    StubPanel::body(ctx, "События###uiEventsPanel", "Временные ивенты и награды.");
}

}  // namespace EventsPanel
