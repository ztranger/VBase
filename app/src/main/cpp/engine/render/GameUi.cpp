#include "engine/render/GameUi.h"

#include <cstdint>
#include <vector>

#include "imgui.h"

#include "engine/assets/AssetSource.h"
#include "engine/core/Log.h"
#include "engine/core/Renderer.h"
#include "engine/render/UiSkin.h"
#include "engine/render/ui/UiShell.h"

namespace GameUi {
namespace {

UiSkin::Assets g_skin;

}  // namespace

void loadFont(AssetSource& assets) {
    ImGuiIO& io = ImGui::GetIO();
    static std::vector<uint8_t> data;
    if (data.empty()) assets.read("fonts/ui.ttf", data);
    if (data.empty()) {
        LOGW("UI-шрифт fonts/ui.ttf не найден — кириллица не отрисуется");
        return;
    }
    LOGI("UI-шрифт загружен (%d байт, кириллица)", (int)data.size());

    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    io.Fonts->Clear();
    io.Fonts->AddFontFromMemoryTTF(data.data(), (int)data.size(), 18.0f, &cfg,
                                   io.Fonts->GetGlyphRangesCyrillic());
}

void loadSkin(Renderer& renderer, AssetSource& assets) {
    if (!UiSkin::load(renderer, assets, g_skin)) {
        LOGW("GameUi: skin не загружен — стандартный вид ImGui");
    }
    UiShell::loadLoadingAssets(renderer, assets);

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigWindowsResizeFromEdges = true;
    ImGuiStyle& style = ImGui::GetStyle();
    style.TouchExtraPadding = ImVec2(16.0f, 16.0f);
    style.WindowMinSize = ImVec2(280.0f, 180.0f);
}

void unloadSkin(Renderer& renderer) {
    UiShell::unloadLoadingAssets(renderer);
    UiSkin::unload(renderer, g_skin);
}

void build(GameUiState& state, Scene& scene) { UiShell::build(state, scene, g_skin); }

bool gameplayActive() { return UiShell::gameplayActive(); }

UiMode mode() { return UiShell::mode(); }

void requestLoadingScreen() { UiShell::setMode(UiMode::Loading); }

}  // namespace GameUi
