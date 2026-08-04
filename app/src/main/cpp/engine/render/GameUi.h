#pragma once

// Фасад игрового GUI: шрифт/skin + делегирование в UiShell (режимы, панели, диалоги).
// Живёт рядом с платформенным слоем (не в Scene). Вызывается из RenderFrame::ui.
// Иерархия: docs/UI_SYSTEM.md.

class Scene;
class Renderer;
struct AssetSource;

// Состояние, которым владеет приложение (Android / десктоп), не игровой слой.
struct GameUiState {
    float fps = 0.0f;
    char joinIp[64] = "127.0.0.1";

    bool vulkanAvailable = false;
    int backend = 0;         // 0 = OpenGL, 1 = Vulkan
    int requestBackend = -1; // -1 = нет
};

namespace GameUi {

void loadFont(AssetSource& assets);
void loadSkin(Renderer& renderer, AssetSource& assets);
void unloadSkin(Renderer& renderer);
void build(GameUiState& state, Scene& scene);

// Активен ли игровой ввод (бой без модалки). Платформа гейтит WASD/тач-стики по нему,
// чтобы в меню/лоадинге/под диалогом герой и камера не управлялись пустыми зонами экрана.
bool gameplayActive();

// Стартовать на экране лоадинга (десктопный флаг --loading). Одноразово при запуске.
void requestLoadingScreen();

}  // namespace GameUi
