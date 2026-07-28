#pragma once

// Построение игрового GUI (Dear ImGui), общее для Android и десктопа.
// Живёт рядом с платформенным слоем (не в Scene/Character), чтобы не тянуть
// ImGui в платформонезависимое ядро. Вызывается из RenderFrame::ui, между
// ImGui NewFrame и Render.

class Scene;

// Небольшое состояние панели, которым владеет приложение, а не игровой слой.
// И Android, и десктоп держат экземпляр у себя и передают его в build().
struct GameUiState {
    float fps = 0.0f;               // сглаженный FPS (считает приложение)
    float lightAngle = 0.9f;        // угол направленного света (слайдер)
    char joinIp[64] = "127.0.0.1";  // адрес сервера для Join
};

namespace GameUi {

// Собрать виджеты кадра. Тянет только imgui + Scene, ничего платформенного.
void build(GameUiState& state, Scene& scene);

}  // namespace GameUi
