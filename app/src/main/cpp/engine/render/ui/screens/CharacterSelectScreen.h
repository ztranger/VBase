#pragma once

#include "engine/render/ui/UiShell.h"

// Экран выбора персонажа: список ростера + «В бой»/«Назад». 3D-модель выбранного персонажа
// рисует Scene (renderCharacterPreview) позади этой ImGui-панели — см. главный цикл платформы.
namespace CharacterSelectScreen {

void draw(UiShell::Ctx& ctx);

}  // namespace CharacterSelectScreen
