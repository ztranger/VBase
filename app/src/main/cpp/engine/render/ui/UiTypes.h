#pragma once

// Типы иерархии UI: Mode → Panel (hub) → Floating → Dialog.
// См. docs/UI_SYSTEM.md.

#include <functional>

enum class UiMode {
    Loading,
    MainMenu,
    Battle,
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
