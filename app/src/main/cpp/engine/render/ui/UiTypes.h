#pragma once

// Типы иерархии UI: Mode → Panel (hub) → Floating → Dialog.
// См. docs/UI_SYSTEM.md.

#include <functional>

enum class UiMode {
    Loading,
    MainMenu,
    Lobby,           // создать/найти игру, готовность, команды (кооп/PvP) — между меню и боем
    CharacterSelect,
    Battle,
};

// Оверлей поверх базового режима (обычно Battle): не заменяет режим, а рисуется сверх него —
// поэтому рендер-путь мира сохраняется. См. docs/UI_SYSTEM.md.
enum class UiOverlay {
    None,
    Pause,    // пауза (продолжить / в меню)
    Results,  // итог матча (победа/поражение, награды -> в меню/лобби)
};

// Какой 3D-путь рисует платформа под текущим режимом (одна таблица вместо дубль-тернара в
// desktop/platform main). Оверлеи Pause/Results не меняют путь — они поверх Battle (World).
enum class RenderPath {
    MenuBackdrop,      // меню/лобби/лоадинг — пустой фон
    CharacterPreview,  // экран выбора — 3D-превью героя
    World,             // бой — игровой мир
};

// Взаимоисключающие разделы главного меню (hub).
enum class MainMenuPanel {
    Home,
    Inventory,
    Quests,
    Shop,
    Events,
};

enum class DialogResult {
    None,
    Ok,
    Yes,
    No,
    Cancel,
};

using DialogCallback = std::function<void(DialogResult)>;
