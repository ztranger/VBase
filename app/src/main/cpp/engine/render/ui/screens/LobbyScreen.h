#pragma once

#include "engine/render/ui/UiShell.h"

// Экран входа в бой (staging / Lobby). Поглощает бывший CharacterSelect: слева выбор героя
// со статами, справа read-only брифинг уже выбранной сцены (цель / волны / режим / HP ядра),
// снизу «Назад» / «В бой». 3D-превью выбранного героя рисует Scene (renderCharacterPreview)
// позади панелей — режим Lobby использует рендер-путь CharacterPreview (см. UiShell::renderPath).
// Первый заход — соло; зона пати/ready (кооп) — вторым заходом. См. docs/NEXT_STEPS.md «Lobby».
namespace LobbyScreen {

void draw(UiShell::Ctx& ctx);

}  // namespace LobbyScreen
