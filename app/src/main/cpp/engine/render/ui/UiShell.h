#pragma once

#include "imgui.h"

#include "engine/render/GameUi.h"
#include "engine/render/UiSkin.h"
#include "engine/render/ui/Dialogs.h"
#include "engine/render/ui/UiTypes.h"

class Scene;
class Renderer;
struct AssetSource;

namespace UiShell {

// Общий контекст кадра для screens / panels / windows.
struct Ctx {
    GameUiState& state;
    Scene& scene;
    const UiSkin::Assets& skin;

    bool btn(const char* label, const ImVec2& size = ImVec2(0, 0)) const;
    bool beginPanel(const char* name, bool* p_open = nullptr,
                    ImGuiWindowFlags flags = 0) const;
    void endPanel() const;
};

void setMode(UiMode mode);
UiMode mode();

void setPanel(MainMenuPanel panel);
MainMenuPanel panel();

void showDebug(bool open);
bool isDebugOpen();
void toggleDebug();

void pushOk(const char* title, const char* text, DialogCallback cb = {});
void pushYesNo(const char* title, const char* text, DialogCallback cb = {});
bool hasModal();

// Ассеты лоадинга (текстуры). Вызывать из GameUi::loadSkin / unloadSkin.
void loadLoadingAssets(Renderer& renderer, AssetSource& assets);
void unloadLoadingAssets(Renderer& renderer);
bool hasLoadingArt();

void build(GameUiState& state, Scene& scene, const UiSkin::Assets& skin);

}  // namespace UiShell
