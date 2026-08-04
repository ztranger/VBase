# UI system (VBase)

Иерархия GUI поверх Dear ImGui + [`UiSkin`](UI_SKIN.md). Фасад для платформы —
`GameUi::{loadFont,loadSkin,unloadSkin,build}`; оркестрация — `engine/render/ui/UiShell`.

## Иерархия

| Уровень | Правило | Примеры |
|---------|---------|---------|
| **UiMode** | ровно один | `Loading`, `MainMenu`, `Battle` |
| **Panel (hub)** | 0…1 на экран, взаимоисключающие | `Home`, `Inventory`, `Quests`, `Shop`, `Events` |
| **Floating** | show/hide, несколько сразу | `Debug`, `BuildingInfo` |
| **DialogStack** | модалки поверх всего | Ok, YesNo |

```
GameUi::build
  └─ UiShell::build
       ├─ switch(mode) → LoadingScreen | MainMenuScreen | BattleScreen
       │     MainMenu: chrome + active panel + Debug(float)
       │     Battle:   HUD + build + BuildingInfo + Debug + joysticks
       └─ DialogStack::draw (всегда сверху)
```

Типы: `engine/render/ui/UiTypes.h`.

## API (кратко)

```cpp
UiShell::setMode(UiMode::Battle);
UiShell::setPanel(MainMenuPanel::Shop);   // только смысл в MainMenu
UiShell::showDebug(true);
UiShell::pushOk("Сеть", "Ошибка", [](DialogResult r){ ... });
UiShell::pushYesNo("Выход", "Покинуть бой?", [](DialogResult r){
    if (r == DialogResult::Yes) UiShell::setMode(UiMode::MainMenu);
});
```

`GameUiState.showLoadingPreview` при старте (`--loading`) переводит shell в
`UiMode::Loading` один раз; далее флаг синхронизируется с режимом.

## Как добавить раздел главного меню

1. Добавить значение в `enum class MainMenuPanel` (`UiTypes.h`).
2. Файл `engine/render/ui/panels/YourPanel.cpp` + `draw(UiShell::Ctx&)`.
3. Ветка в `MainMenuScreen::draw` + кнопка в chrome (`MainMenuScreen.cpp`).
4. Запись в CMake (`desktop/CMakeLists.txt` и `app/src/main/cpp/CMakeLists.txt`).

Не создавай новый `UiMode` для магазина/инвентаря — это hub panel.

## Как добавить окно в бою

1. `engine/render/ui/windows/YourWindow.cpp`.
2. Вызов из `BattleScreen::draw`.
3. CMake.

## Как показать диалог

Только через `UiShell::pushOk` / `pushYesNo`. Не разводить голые `BeginPopupModal`
вне `Dialogs.cpp` — иначе сломается стек и затемнение.

Callback не должен захватывать стековые ссылки (`Ctx&`) — только указатели на
долгоживущие объекты (`Scene*`) или копируемые значения.

## Каталог

```
engine/render/ui/
  UiTypes.h  UiShell.*  Dialogs.*
  screens/   LoadingScreen.*  MainMenuScreen.*  BattleScreen.*
  panels/    HomePanel.*   (+ stubs Inventory/Quests/Shop/Events)
  windows/   DebugPanel.*  BuildingInfo.*
```

## Связанные доки

- [UI_SKIN.md](UI_SKIN.md) — 9-slice / кнопки
- [UI_PALETTE.md](UI_PALETTE.md) — цвета
- [ARCHITECTURE.md](ARCHITECTURE.md) §3
