#include "engine/render/ui/UiShell.h"

#include "imgui.h"

#include "engine/render/ui/screens/BattleScreen.h"
#include "engine/render/ui/screens/CharacterSelectScreen.h"
#include "engine/render/ui/screens/LoadingScreen.h"
#include "engine/render/ui/screens/MainMenuScreen.h"

namespace UiShell {
namespace {

UiMode g_mode = UiMode::MainMenu;
MainMenuPanel g_panel = MainMenuPanel::Home;
bool g_debugOpen = true;
UiDialogs::Stack g_dialogs;

}  // namespace

bool Ctx::btn(const char* label, const ImVec2& size) const {
    if (skin.ready) return UiSkin::Button(label, skin, size);
    return ImGui::Button(label, size);
}

bool Ctx::beginPanel(const char* name, bool* p_open, ImGuiWindowFlags flags) const {
    if (skin.ready) return UiSkin::BeginPanel(name, skin, p_open, flags);
    return ImGui::Begin(name, p_open, flags);
}

bool Ctx::beginPanelRect(const char* name, const ImVec2& pos, const ImVec2& size, bool* p_open,
                         ImGuiWindowFlags extraFlags) const {
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                   extraFlags;
    return beginPanel(name, p_open, flags);
}

void Ctx::endPanel() const {
    if (skin.ready) UiSkin::EndPanel();
    else ImGui::End();
}

float navBarHeight() { return ImGui::GetFontSize() * 3.0f; }
float uiMargin() { return ImGui::GetFontSize() * 0.9f; }

void menuContentRect(ImVec2& pos, ImVec2& size) {
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    const float m = uiMargin();  // поля от краёв экрана и до нав-бара (в единицах шрифта)
    pos = ImVec2(m, m);
    size = ImVec2(disp.x - m * 2.0f, disp.y - navBarHeight() - m * 2.0f);
    if (size.x < 200.0f) size.x = 200.0f;  // страховка на крошечном экране
    if (size.y < 160.0f) size.y = 160.0f;
}

void setMode(UiMode mode) {
    g_mode = mode;
    if (mode == UiMode::MainMenu) {
        // возвращаемся на Home, если ушли в Loading; panel иначе сохраняем
    }
}

UiMode mode() { return g_mode; }

void setPanel(MainMenuPanel panel) { g_panel = panel; }

MainMenuPanel panel() { return g_panel; }

void showDebug(bool open) { g_debugOpen = open; }

bool isDebugOpen() { return g_debugOpen; }

void toggleDebug() { g_debugOpen = !g_debugOpen; }

void pushOk(const char* title, const char* text, DialogCallback cb) {
    g_dialogs.pushOk(title, text, std::move(cb));
}

void pushYesNo(const char* title, const char* text, DialogCallback cb) {
    g_dialogs.pushYesNo(title, text, std::move(cb));
}

bool hasModal() { return g_dialogs.hasModal(); }

bool gameplayActive() { return g_mode == UiMode::Battle && !g_dialogs.hasModal(); }

void loadLoadingAssets(Renderer& renderer, AssetSource& assets) {
    LoadingScreen::load(renderer, assets);
}

void unloadLoadingAssets(Renderer& renderer) { LoadingScreen::unload(renderer); }

bool hasLoadingArt() { return LoadingScreen::hasArt(); }

void build(GameUiState& state, Scene& scene, const UiSkin::Assets& skin) {
    Ctx ctx{state, scene, skin};

    switch (g_mode) {
        case UiMode::Loading: {
            LoadingScreen::draw();
            ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(0.65f);
            if (ImGui::Begin("##loadingPreviewCtrl", nullptr,
                             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoSavedSettings)) {
                ImGui::TextUnformatted("Превью лоадинга");
                if (ctx.btn("Скрыть")) setMode(UiMode::MainMenu);
                ImGui::SameLine();
                ImGui::TextDisabled("(или --loading)");
            }
            ImGui::End();
            break;
        }
        case UiMode::MainMenu:
            MainMenuScreen::draw(ctx);
            break;
        case UiMode::CharacterSelect:
            CharacterSelectScreen::draw(ctx);
            break;
        case UiMode::Battle:
            BattleScreen::draw(ctx);
            break;
    }

    g_dialogs.draw(skin);
}

}  // namespace UiShell
