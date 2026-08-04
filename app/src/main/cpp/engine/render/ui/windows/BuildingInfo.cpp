#include "engine/render/ui/windows/DebugPanel.h"

#include <cfloat>
#include <string>

#include "imgui.h"

#include "game/BuildingConfig.h"
#include "game/Scene.h"

namespace BuildingInfoWindow {

void draw(UiShell::Ctx& ctx) {
    int selType = ctx.scene.selectedEntityType();
    const BuildingInfo* info = ctx.scene.selectedInfo();
    if (selType < 0 || info == nullptr) return;

    ImGui::SetNextWindowPos(ImVec2(400, 90), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(440, 320), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(280, 180), ImVec2(FLT_MAX, FLT_MAX));
    std::string title = (info->name.empty() ? "Здание" : info->name) + "###buildInfoPanel";
    bool open = true;
    if (!ctx.beginPanel(title.c_str(), &open)) {
        ctx.endPanel();
        if (!open) ctx.scene.clearSelection();
        return;
    }

    if (!info->desc.empty()) ImGui::TextWrapped("%s", info->desc.c_str());
    ImGui::Separator();
    switch ((EntityType)selType) {
        case EntityType::Generator:
            ImGui::Text("Генерация: %.0f / сек", (double)info->rate);
            break;
        case EntityType::Storage:
            ImGui::Text("Ёмкость: %.0f", (double)info->cap);
            ImGui::Text("Сейчас: %.0f", (double)ctx.scene.selectedAux());
            break;
        case EntityType::Spawner:
            ImGui::Text("Интервал: %.1f с", (double)info->rate);
            ImGui::Text("Максимум врагов: %.0f", (double)info->cap);
            break;
        case EntityType::Tower:
            ImGui::Text("Урон: %.0f / выстрел", (double)info->damage);
            ImGui::Text("Радиус: %.1f", (double)info->range);
            ImGui::Text("Интервал: %.2f с", (double)info->rate);
            break;
        case EntityType::Core: {
            float ch = ctx.scene.coreHp();
            ImGui::Text("Здоровье: %.0f / %.0f", (double)(ch < 0.0f ? 0.0f : ch),
                        (double)info->hp);
            break;
        }
        default:
            break;
    }
    if (ctx.btn("Закрыть")) ctx.scene.clearSelection();
    ctx.endPanel();
    if (!open) ctx.scene.clearSelection();
}

}  // namespace BuildingInfoWindow
