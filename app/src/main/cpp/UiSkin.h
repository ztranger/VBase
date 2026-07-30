#pragma once

// Тонкий skin-слой поверх Dear ImGui: 9-slice панели и кнопки из текстур.
// Логика окон/кликов/шрифтов остаётся у ImGui — меняется только отрисовка.

#include "imgui.h"

#include "Renderer.h"
#include "Texture.h"

struct AssetSource;

namespace UiSkin {

// Набор текстур скина. ImTextureID — бэкенд-зависимый (GL: GLuint, VK: DescriptorSet).
struct Assets {
    ImTextureID panel = ImTextureID_Invalid;
    ImTextureID btnNormal = ImTextureID_Invalid;
    ImTextureID btnHover = ImTextureID_Invalid;
    ImTextureID btnActive = ImTextureID_Invalid;

    TextureHandle panelHandle = 0;
    TextureHandle btnNormalHandle = 0;
    TextureHandle btnHoverHandle = 0;
    TextureHandle btnActiveHandle = 0;

    float panelTexW = 64.0f;
    float panelTexH = 64.0f;
    float panelBorderTexels = 16.0f;  // толщина рамки в текселях атласа 9-slice
    bool ready = false;
};

// Загрузить ui/*.png или сгенерировать процедурные текстуры. Нужен живой ImGui-бэкенд
// (после ImGui_Impl*_Init): Vulkan регистрирует дескрипторы через getImGuiTexture.
bool load(Renderer& renderer, AssetSource& assets, Assets& out);

// Снять ImGui-дескрипторы (важно для Vulkan до ImGui_ImplVulkan_Shutdown).
void unload(Renderer& renderer, Assets& assets);

// Окно: 9-slice рамка + полоса title + контент в BeginChild (с inset).
// name — как у ImGui::Begin ("Title###id"); p_open — крестик закрытия в title.
bool BeginPanel(const char* name, const Assets& skin, bool* p_open = nullptr,
                ImGuiWindowFlags flags = 0);
void EndPanel();

// Кнопка: InvisibleButton (логика) + своя текстура (вид) + подпись шрифтом ImGui.
bool Button(const char* label, const Assets& skin, const ImVec2& size = ImVec2(0, 0));

}  // namespace UiSkin
