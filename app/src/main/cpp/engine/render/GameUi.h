#pragma once

// Построение игрового GUI (Dear ImGui), общее для Android и десктопа.
// Живёт рядом с платформенным слоем (не в Scene/Character), чтобы не тянуть
// ImGui в платформонезависимое ядро. Вызывается из RenderFrame::ui, между
// ImGui NewFrame и Render.

class Scene;
class Renderer;
struct AssetSource;

// Небольшое состояние панели, которым владеет приложение, а не игровой слой.
// И Android, и десктоп держат экземпляр у себя и передают его в build().
struct GameUiState {
    float fps = 0.0f;               // сглаженный FPS (считает приложение)
    char joinIp[64] = "127.0.0.1";  // адрес сервера для Join

    // Переключение графического бэкенда через панель.
    bool vulkanAvailable = false;   // доступен ли Vulkan (иначе кнопка disabled)
    int backend = 0;                // текущий бэкенд: 0 = OpenGL, 1 = Vulkan (ставит платформа)
    int requestBackend = -1;        // запрошенный кнопкой (-1 = нет; читает и сбрасывает платформа)
};

namespace GameUi {

// Загрузить UI-шрифт с кириллицей (assets/fonts/ui.ttf) в текущий контекст ImGui.
// Вызывать после ImGui::CreateContext и ДО инициализации бэкенда (сборки атласа).
// Байты шрифта держатся живыми весь процесс (нужны атласу).
void loadFont(AssetSource& assets);

// Загрузить skin (9-slice + кнопки). После ImGui_Impl*_Init. При пересоздании
// рендера — снова; перед Shutdown — unloadSkin.
void loadSkin(Renderer& renderer, AssetSource& assets);
void unloadSkin(Renderer& renderer);

// Собрать виджеты кадра. Тянет только imgui + Scene, ничего платформенного.
void build(GameUiState& state, Scene& scene);

}  // namespace GameUi
