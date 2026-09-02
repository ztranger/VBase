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
    // «Нормальное» окно: приклеено к прямоугольнику (пиксели), без драга/ресайза/сейва.
    // Для игровых панелей — чтобы не таскались как debug-окна. extraFlags — доп. флаги
    // (напр. AlwaysAutoResize, тогда size задаёт только позицию/ширину).
    bool beginPanelRect(const char* name, const ImVec2& pos, const ImVec2& size,
                        bool* p_open = nullptr, ImGuiWindowFlags extraFlags = 0) const;
    void endPanel() const;
};

// Метрики UI в единицах шрифта — масштабируются под DPI (платформа масштабирует шрифт).
// Звать только внутри кадра ImGui (нужен активный шрифт). ~54/16 px на десктопе (font 18).
float navBarHeight();  // высота нижнего нав-бара главного меню (общая для бара и раскладки)
float uiMargin();      // стандартное поле от краёв экрана и между окнами

// Прямоугольник контент-области меню (над нав-баром, с полями). Панели разделов
// (Главная/Инвентарь/…) якорятся сюда — заполняют область, а не висят коробочкой в углу.
void menuContentRect(ImVec2& pos, ImVec2& size);

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

// true только когда идёт бой и нет модалки. По нему платформа гейтит игровой ввод:
// в меню/лоадинге/под диалогом WASD и тач-стики не должны двигать героя/камеру.
bool gameplayActive();

// Ассеты лоадинга (текстуры). Вызывать из GameUi::loadSkin / unloadSkin.
void loadLoadingAssets(Renderer& renderer, AssetSource& assets);
void unloadLoadingAssets(Renderer& renderer);
bool hasLoadingArt();

void build(GameUiState& state, Scene& scene, const UiSkin::Assets& skin);

}  // namespace UiShell
