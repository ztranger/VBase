#include "engine/render/ui/windows/BuildingInfo.h"

#include <string>

#include "imgui.h"

#include "game/BuildingConfig.h"
#include "game/Scene.h"

namespace BuildingInfoWindow {

void draw(UiShell::Ctx& ctx) {
    int selType = ctx.scene.selectedEntityType();
    if (selType < 0) return;
    const bool defense = ctx.scene.selectedIsDefense();  // Tower/Trap — данные из ростера towers.cfg
    const BuildingInfo* info = ctx.scene.selectedInfo(); // null для Trap (не в BuildingConfig)
    if (!defense && info == nullptr) return;

    // Заякорено в правом верхнем углу (не таскается): панель инфо о выбранном здании.
    // Метрики в единицах шрифта — масштабируются под DPI.
    const float font = ImGui::GetFontSize();
    const float w = font * 20.0f, h = font * 13.0f;
    const ImVec2 pos(ImGui::GetIO().DisplaySize.x - w - UiShell::uiMargin(), font * 5.5f);
    std::string name = defense ? ctx.scene.selectedDefenseName()
                               : (info && !info->name.empty() ? info->name : std::string("Здание"));
    if (name.empty()) name = "Постройка";
    std::string title = name + "###buildInfoPanel";
    bool open = true;
    if (!ctx.beginPanelRect(title.c_str(), pos, ImVec2(w, h), &open)) {
        ctx.endPanel();
        if (!open) ctx.scene.clearSelection();
        return;
    }

    if (info != nullptr && !info->desc.empty()) ImGui::TextWrapped("%s", info->desc.c_str());
    ImGui::Separator();
    switch ((EntityType)selType) {
        case EntityType::Generator:
            if (info) ImGui::Text("Генерация: %.0f / сек", (double)info->rate);
            break;
        case EntityType::Storage:
            if (info) ImGui::Text("Ёмкость: %.0f", (double)info->cap);
            ImGui::Text("Сейчас: %.0f", (double)ctx.scene.selectedAux());
            break;
        case EntityType::Spawner:
            if (info) {
                ImGui::Text("Интервал: %.1f с", (double)info->rate);
                ImGui::Text("Максимум врагов: %.0f", (double)info->cap);
            }
            break;
        case EntityType::Tower:
        case EntityType::Trap:
            ImGui::Text("Тир: %d / %d", ctx.scene.selectedTier(), ctx.scene.selectedMaxTier());
            break;
        case EntityType::Core: {
            float ch = ctx.scene.coreHp();
            ImGui::Text("Здоровье: %.0f / %.0f", (double)(ch < 0.0f ? 0.0f : ch),
                        info ? (double)info->hp : 0.0);
            break;
        }
        default:
            break;
    }

    // Апгрейд/снос — только для СВОИХ построек. Апгрейд у защиты (Tower/Trap); снос — у защиты
    // и генератора/хранилища. Сервер валидирует авторитетно (ресурс/тир/владение).
    if (defense) {
        ImGui::Separator();
        float upCost = ctx.scene.selectedUpgradeCost();
        if (upCost > 0.0f) {
            bool can = ctx.scene.selectedCanUpgrade();
            ImGui::BeginDisabled(!can);
            std::string lbl = "Улучшить (" + std::to_string((int)upCost) + ")";
            if (ctx.btn(lbl.c_str())) ctx.scene.upgradeSelected();
            ImGui::EndDisabled();
        } else if (ctx.scene.selectedMine()) {
            ImGui::TextDisabled("Максимальный тир");
        }
    }
    if (ctx.scene.selectedDemolishable()) {
        std::string lbl = "Снести (+" + std::to_string((int)ctx.scene.selectedRefund()) + ")";
        if (ctx.btn(lbl.c_str())) {
            ctx.scene.demolishSelected();
            ctx.scene.clearSelection();
        }
    }

    if (ctx.btn("Закрыть")) ctx.scene.clearSelection();
    ctx.endPanel();
    if (!open) ctx.scene.clearSelection();
}

}  // namespace BuildingInfoWindow
