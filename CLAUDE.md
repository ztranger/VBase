# VBase — нативный 3D-движок (Android) + мультиплеер

Учебный нативный 3D-движок на C++ под Android (OpenGL ES 3), выросший до
клиент-серверного мультиплеера с выделенным десктопным сервером и десктопным
клиентом. Комментарии и логи — на русском.

Подробности архитектуры, дизайна неткода, карты файлов и «граблей»:
см. **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)**.
План дальнейших работ (что делаем следующим): **[docs/NEXT_STEPS.md](docs/NEXT_STEPS.md)**.
Как рисовать UI-текстуры под 9-slice: **[docs/UI_SKIN.md](docs/UI_SKIN.md)**.
Цветовая палитра (TD × Orcs Must Die): **[docs/UI_PALETTE.md](docs/UI_PALETTE.md)**.
Ниже — самое нужное для старта.

## Три сборочные цели

| Цель | Где | Как собрать | Что это |
|---|---|---|---|
| Android-приложение | `app/` | Android Studio (Run ▶) | клиент: рендер GLES3 + игра + сеть |
| Выделенный сервер | `server/` | `server\build.bat` (MSVC) | авторитетный сервер, headless |
| Десктоп-клиент | `desktop/` | `desktop\build.bat` (MSVC) | GLFW + desktop GL 3.3, переиспользует ядро |

Нативный код клиента живёт в `app/src/main/cpp/`, разложен по слоям:
`platform/` (Android-точка входа), `engine/{core,render,assets,physics,net}`
(переиспользуемый движок), `game/` (геймплей: Scene, Character, здания, загрузчики).
Include — **квалифицированные** от корня `cpp/` (напр. `#include "engine/net/Net.h"`,
`#include "game/Scene.h"`); корень `cpp/` добавлен в include-пути всех трёх целей.
Сервер и десктоп переиспользуют платформонезависимые файлы (симуляция, сеть, загрузчики,
математика) прямо из этого дерева. Полная карта — ARCHITECTURE §5.

**Сборка из корня:** `build-server.cmd` / `build-desktop.cmd` — тонкие обёртки над
`server\build.bat` / `desktop\build.bat` (те находят свои пути через `%~dp0`, работают
из любой директории). Для Android — по-прежнему Android Studio.

**Запуск из корня:** `run-server.cmd` / `run-desktop.cmd` (лаунчеры). Сами заходят в
`*/build` и вызывают бинарь по полному пути — работают из любой директории и по
двойному клику (важно: ассеты грузятся относительно рабочей папки `*/build`, поэтому
голый симлинк на `.exe` в корне НЕ годится). Аргументы пробрасываются: у сервера
`[port] [assetsDir] [scenePath]`, у десктопа `[serverIp] [assetsDir] [scenePath]`.

## Тулчейн (эта машина, Windows)

- Android: NDK `28.2.13676358`, GameActivity `3.0.5`, `minSdk 30`, `targetSdk 36`,
  namespace `com.hpg.vbase`. Сборка — только через Android Studio (Gradle wrapper
  не генерировали; из консоли мешает блокировка общего кэша `~/.gradle`).
- Десктоп/сервер: MSVC (VS Build Tools 18, `vcvars64.bat`) + bundled CMake —
  всё зашито в `server/build.bat` и `desktop/build.bat`.
- Проверка компиляции нативного кода без Gradle: NDK clang с
  `--target=aarch64-linux-android30 -std=c++20 -fsyntax-only` (см. ARCHITECTURE).

## Вендоренные зависимости (`third_party/`)

`imgui` (GUI), `enet` (UDP), `cgltf` (glTF), `stb` (stb_image), `glfw` (окно на десктопе),
`vulkan` (заголовки Khronos, не линкуются), `jolt` (Jolt Physics v5.6.0 — кинематический
контроллер; подключается родным CMake-таргетом, см. NEXT_STEPS §2.5).
`glew/` — пустой остаток неудачной загрузки, **не используется**, можно удалить.

## Ключевые правила

- **CMake для C-исходников**: в проектах с `.c` (ENet) язык C обязателен —
  `project(... C CXX)`, иначе `.c` молча не компилируются.
- **Заголовок математики — `MathUtil.h`**, НЕ `Math.h` (на Windows столкнулся бы
  со стандартным `<math.h>` из-за регистронезависимой ФС).
- **Не ломать платформонезависимость**: `game/*` и `engine/{core,assets,physics,net}`
  (`Character`, `Scene`, `Net`, `Model`, `Mesh`, `MathUtil`, `Input`, `FollowCamera`,
  `Assets`, `AssetSource`, `CollisionWorld`, …) не должны тянуть Android/GL/ImGui.
  GL/ImGui/Android живут **только** в `engine/render/*` и `platform/`. Рендер и платформа —
  за интерфейсами (`Renderer`, `AssetSource`); физика (Jolt) — за `CollisionWorld`
  (pimpl, Jolt не течёт в заголовок).
- **Физика — кинематический контроллер на Jolt** (`CollisionWorld.*`): статика арены
  (боксы) + капсулы `CharacterVirtual` (гравитация/прыжок/скольжение). `Character::simulate`
  двигает через неё; клиент предсказывает, сервер авторитетно — один код, одна геометрия
  (`.scene` c директивами `collider`). Подключение Jolt — родным CMake-таргетом (см.
  NEXT_STEPS §2.5, готчи там же).
- **Транспорт vs игра разделены**: авторитетная серверная симуляция (сущности + системы:
  движение, экономика, спавнеры, враги, дальше бой) живёт в `game/GameWorld` (БЕЗ ENet).
  `engine/net/Net` (`NetServer`) — чистый транспорт: кормит `GameWorld` вводом, двигает,
  сериализует снапшоты. **Новые игровые системы — в `GameWorld::step`, не в `NetServer::tick`.**
  Частота тика — единый `kTickDt` в `Net.h`. Версию протокола `kProtocolVersion` бампать при
  любом изменении раскладки сетевых структур. Подробности — ARCHITECTURE §4.
- **Два рендер-бэкенда за интерфейсом `Renderer`**: `GlRenderer` (GL ES3 / desktop
  GL3.3) и `VulkanRenderer` (+ динамический загрузчик `VkApi.*`). Vulkan рисует всё:
  окружение (инстансинг, Lit/Unlit/Phong, текстуры), скиннинг (лиса, кости в SSBO),
  HUD, ImGui. Заголовки Vulkan вендорены в `third_party/vulkan/`, функции грузятся
  динамически (desktop: GLFW; Android: dlopen libvulkan) — **libvulkan НЕ линкуется**
  (иначе имена-указатели `VkApi` конфликтуют с экспортами). Бэкенд переключается
  **кнопкой в панели `GameUi`** в рантайме (Vulkan disabled, если недоступен);
  на десктопе окно пересоздаётся (`runClient`), на Android — рендер из того же окна.
  **Проверено запуском на обеих целях** (десктоп и Android: рендер, переключение
  GL↔Vulkan из панели, ImGui под Vulkan).
- **ImGui skin (`UiSkin`)**: панели — 9-slice, кнопки — свои текстуры; логика и шрифт
  ImGui не меняются. Ассеты в `app/src/main/assets/ui/` (`panel.png`, `button_*.png`);
  перегенерация — `python .../ui/gen_ui_skin.py`. Загрузка `GameUi::loadSkin` после
  init бэкенда ImGui, `unloadSkin` перед Shutdown. Подробности — ARCHITECTURE §3 / §6.
