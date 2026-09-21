# UI system (VBase)

Иерархия GUI поверх Dear ImGui + [`UiSkin`](UI_SKIN.md). Фасад для платформы —
`GameUi::{loadFont,loadSkin,unloadSkin,build}`; оркестрация — `engine/render/ui/UiShell`.

## Иерархия

| Уровень | Правило | Примеры |
|---------|---------|---------|
| **UiMode** | ровно один (экран) | `Loading`, `MainMenu`, `Lobby` (вход в бой: герой + брифинг), `Battle` |
| **UiOverlay** | 0…1 поверх режима, не меняет рендер-путь | `None`, `Pause`, `Results` |
| **Panel (hub)** | 0…1 на экран, взаимоисключающие | `Home`, `Inventory`, `Quests`, `Shop`, `Events` |
| **Floating** | show/hide, несколько сразу | `Debug`, `BuildingInfo` |
| **DialogStack** | модалки поверх всего | Ok, YesNo |

```
GameUi::build
  └─ UiShell::build
       ├─ switch(mode) → LoadingScreen | MainMenuScreen | LobbyScreen | BattleScreen
       │     MainMenu: chrome + active panel + Debug(float)
       │     Battle:   HUD + build + BuildingInfo + Debug + joysticks
       ├─ switch(overlay) → Pause | Results   (поверх режима; рендер-путь мира сохраняется)
       └─ DialogStack::draw (всегда сверху)
```

Типы: `engine/render/ui/UiTypes.h`.

## Навигация — единый авторитет (UiShell)

Один источник правды о том, где мы. Раньше это было размазано по ~6 файлам, плюс дубль-тернар
рендер-пути в `desktop/main.cpp` и `platform/main.cpp` — теперь всё из shell:

```cpp
UiShell::setMode(UiMode::Battle);   // сменить экран: запомнит prevMode, снимет оверлей,
                                    //   а вход в MainMenu вернёт вкладку хаба на Home
UiShell::mode();  UiShell::prevMode();
UiShell::back();                    // назад в предыдущий экран (снимает оверлей); дефолт -> MainMenu
UiShell::setOverlay(UiOverlay::Pause);  UiShell::overlay();   // оверлей поверх боя
UiShell::renderPath();              // RenderPath: World | CharacterPreview | MenuBackdrop
UiShell::gameplayActive();          // ввод в мир: Battle && overlay==None && !hasModal()
```

**Рендер-путь — одна таблица `mode → RenderPath`** (`UiShell::renderPath()`, форвардится через
`GameUi::renderPath()`). Платформа (`desktop/main.cpp`, `platform/main.cpp`) выбирает 3D-путь
`switch`-ем по нему — **не** повторяет тернар по `mode()`. Оверлеи `Pause`/`Results` путь не
меняют (рисуются поверх `Battle`, мир под ними продолжает рендериться).

**Оверлей vs Mode:** новый экран (лобби, выбор) — это `UiMode`. Пауза/итог матча/модальные
надстройки поверх боя — это `UiOverlay` (мир под ними виден). Не заводи `UiMode` для паузы.

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

Десктопный флаг `--loading` зовёт `GameUi::requestLoadingScreen()` один раз при
старте (`UiShell::setMode(UiMode::Loading)`); дальше режимом рулит только shell.

## Окна: заякоренные, не таскаются (не debug-залипухи)

Игровые панели — **«нормальные» окна**: приклеены к прямоугольнику, без свободного
перетаскивания/ресайза/сейва позиции. Открывать через `Ctx::beginPanelRect(name, pos, size,
p_open=nullptr, extraFlags=0)` — он ставит `SetNextWindowPos/Size(..., Always)` и флаги
`NoMove|NoResize|NoCollapse|NoSavedSettings`. Скин уважает `NoMove` (drag за title выключается).
НЕ ставить панели через `beginPanel` + `SetNextWindowPos(..., FirstUseEver)` — так они плавают
коробочкой в углу и таскаются. Исключение — **`DebugPanel`**: это дебаг-инструмент, ему
перетаскивание оставлено намеренно.

- **Главное меню** (Home + стабы): панель заполняет область над нав-баром —
  `UiShell::menuContentRect(pos, size)`. Метрики — в **единицах шрифта** (масштаб под DPI, т.к.
  платформа ставит `style.FontScaleDpi`): `UiShell::navBarHeight()` (≈3× шрифта) и
  `UiShell::uiMargin()` (≈0.9× шрифта); прочие панели считают позиции/размеры от `ImGui::GetFontSize()`.
- **Выбор персонажа**: левая колонка на всю высоту (превью справа за ней).
- **Бой**: HUD — верх-лево (`NoMove`), панель стройки — лево под HUD (авто-высота,
  `AlwaysAutoResize`), инфо о здании — верх-право.
- Авто-высота под контент: передать `extraFlags=ImGuiWindowFlags_AlwaysAutoResize` (тогда
  `size` задаёт только позицию/ширину-хинт).

## Как добавить раздел главного меню

1. Добавить значение в `enum class MainMenuPanel` (`UiTypes.h`).
2. Файл `engine/render/ui/panels/YourPanel.cpp` + `draw(UiShell::Ctx&)`.
3. Ветка в `MainMenuScreen::draw` + кнопка в chrome (`MainMenuScreen.cpp`).
4. Запись в CMake (`desktop/CMakeLists.txt` и `app/src/main/cpp/CMakeLists.txt`).

Не создавай новый `UiMode` для магазина/инвентаря — это hub panel.

## Как добавить оверлей (пауза/итог/…)

1. Значение в `enum class UiOverlay` (`UiTypes.h`).
2. Рисующая функция + ветка в `switch(overlay)` внутри `UiShell::build` (сейчас там же лежат
   `drawPauseOverlay`/`drawResultsOverlay` — заглушки, наполнение в большом проходе по окнам).
3. Открывать через `UiShell::setOverlay(...)`, закрывать `setOverlay(UiOverlay::None)`.

Оверлей не меняет `renderPath()` — мир под ним продолжает рендериться. Для полноэкранной смены
контекста (не поверх боя) заводи `UiMode`, а не оверлей.

## Как добавить окно в бою

1. `engine/render/ui/windows/YourWindow.cpp`.
2. Вызов из `BattleScreen::draw`.
3. CMake.

## Как показать диалог / тост

Модалки — только через shell, не разводить голые `BeginPopupModal` вне `Dialogs.cpp`
(иначе сломается стек и затемнение):

```cpp
UiShell::pushOk("Сеть", "Ошибка", cb);                 // одна кнопка OK
UiShell::pushYesNo("Выход", "Покинуть бой?", cb);      // Да / Нет
UiShell::pushDialog("Режим", "Выбери режим:",          // произвольные кнопки
    {{"Кооп", DialogResult::Yes}, {"PvP", DialogResult::No}, {"Отмена", DialogResult::Cancel}}, cb);
```

`pushOk`/`pushYesNo` — пресеты поверх `pushDialog` (набор `UiDialogs::Button{label, result}`).
Крестик окна = `DialogResult::Cancel`. Диалоги — стек: новый рисуется поверх, в т.ч. поверх
оверлея (пауза).

**Тосты** — недолгие НЕблокирующие плашки (успех/ошибка/инфо), сами гаснут, `hasModal()` их
не считает, ввод в мир не гейтят:

```cpp
UiShell::pushToast("Здание построено", UiDialogs::Toast::Kind::Success);
UiShell::pushToast("Недостаточно ресурса", UiDialogs::Toast::Kind::Danger, 2.0f);
```

Callback не должен захватывать стековые ссылки (`Ctx&`) — только указатели на
долгоживущие объекты (`Scene*`) или копируемые значения.

## Каталог

```
engine/render/ui/
  UiTypes.h  UiShell.*  UiPalette.h  Dialogs.*
  screens/   LoadingScreen.*  MainMenuScreen.*  LobbyScreen.*  BattleScreen.*
  panels/    HomePanel.*  InventoryPanel.*  QuestsPanel.*  ShopPanel.*  EventsPanel.*  StubPanel.h
             (каждый раздел хаба = свой файл; StubPanel.h — общее тело заглушки)
  windows/   DebugPanel.*  BuildingInfo.*
```

`Lobby` — экран входа в бой (`screens/LobbyScreen.*`): слева выбор героя со статами, справа
read-only брифинг сцены; рендер-путь `CharacterPreview` (3D-герой за панелями). Соло-first;
мини-карта и кооп-пати/ready — вторым заходом (см. NEXT_STEPS). Оверлеи `Pause`/`Results` пока —
**инлайн-заглушки** в `UiShell.cpp` (`drawPauseOverlay`/`drawResultsOverlay`); при наполнении
переедут в `overlays/`.

## Связанные доки

- [UI_SKIN.md](UI_SKIN.md) — 9-slice / кнопки
- [UI_PALETTE.md](UI_PALETTE.md) — цвета
- [ARCHITECTURE.md](ARCHITECTURE.md) §3
