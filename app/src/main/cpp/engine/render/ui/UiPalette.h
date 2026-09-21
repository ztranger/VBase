#pragma once

#include "imgui.h"

// Единая палитра UI (TD × Orcs Must Die), кодовое зеркало docs/UI_PALETTE.md. Раньше цвета были
// рассыпаны magic-хексами по UiSkin/Dialogs/Loading/экранам — на большом пласте окон это разъедется.
// Все окна/скин/оверлеи берут цвета ОТСЮДА. IM_COL32 (ImU32) — основной формат; v4() для мест,
// где ImGui хочет ImVec4 (PushStyleColor/TextColored).
namespace UiPalette {

// Нейтрали (тёплый тёмный камень).
inline constexpr ImU32 Void        = IM_COL32(26, 20, 16, 255);   // фон/центр 9-slice
inline constexpr ImU32 Panel       = IM_COL32(42, 34, 28, 255);   // заливка панели
inline constexpr ImU32 TitleBar    = IM_COL32(51, 40, 32, 255);   // полоса заголовка
inline constexpr ImU32 Stone       = IM_COL32(74, 63, 53, 255);   // рамка тёмная
inline constexpr ImU32 StoneLight  = IM_COL32(122, 106, 88, 255); // рамка светлый край

// Текст.
inline constexpr ImU32 Text        = IM_COL32(242, 230, 212, 255); // основной
inline constexpr ImU32 TextDim     = IM_COL32(184, 169, 148, 255); // вторичный
inline constexpr ImU32 TextOff     = IM_COL32(110, 100, 88, 255);  // disabled
inline constexpr ImU32 TextOnAmber = IM_COL32(26, 20, 16, 255);    // тёмный текст на янтаре

// Акценты (кнопки/смысл). Один главный CTA — янтарь; остальные точечно.
inline constexpr ImU32 Amber       = IM_COL32(232, 161, 58, 255);  // primary / CTA (normal)
inline constexpr ImU32 AmberHover  = IM_COL32(240, 185, 90, 255);  // hover
inline constexpr ImU32 AmberPress  = IM_COL32(196, 132, 34, 255);  // pressed / active
inline constexpr ImU32 Copper      = IM_COL32(196, 165, 116, 255); // обводка рамки/выделения
inline constexpr ImU32 Success     = IM_COL32(61, 158, 90, 255);   // ресурс/хил/успех
inline constexpr ImU32 Danger      = IM_COL32(196, 60, 50, 255);   // угроза/удалить
inline constexpr ImU32 Magic       = IM_COL32(168, 85, 247, 255);  // магия
inline constexpr ImU32 Info        = IM_COL32(212, 165, 116, 255); // подсказки (не голубой)
inline constexpr ImU32 Gold        = IM_COL32(255, 211, 106, 255); // награда/лут/выделенное золото

// Домешать альфу к цвету палитры (для полупрозрачных заливок/линий).
inline ImU32 withAlpha(ImU32 c, int a) {
    return (c & 0x00FFFFFFu) | ((ImU32)(a & 0xFF) << IM_COL32_A_SHIFT);
}
// ImU32 -> ImVec4 (0..1) для ImGui::PushStyleColor/TextColored.
inline ImVec4 v4(ImU32 c) {
    return ImVec4(((c >> IM_COL32_R_SHIFT) & 0xFF) / 255.0f, ((c >> IM_COL32_G_SHIFT) & 0xFF) / 255.0f,
                  ((c >> IM_COL32_B_SHIFT) & 0xFF) / 255.0f, ((c >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f);
}

}  // namespace UiPalette
