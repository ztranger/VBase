# VBase — нативный 3D-движок (Android) + мультиплеер

Нативный 3D-движок на C++ под Android (OpenGL ES 3), выросший до клиент-серверного
мультиплеера с выделенным десктопным сервером и десктопным клиентом. Комментарии и логи
— на русском.

**Цель — продакшн-качество, не учебный прототип.** Практические следствия: сервер
авторитетен и **валидирует весь недоверенный ввод** (сетевые пакеты, конфиги, ассеты, CLI),
сборки воспроизводимы, новое поведение покрывается headless-тестами (`--selftest`), дефекты
чинятся, а не обходятся. План стабилизации/хардненинга и статус: **[docs/PROJECT_REVIEW.md](docs/PROJECT_REVIEW.md)**.

Подробности архитектуры, дизайна неткода, карты файлов и «граблей»:
см. **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)**.
План дальнейших работ (что делаем следующим): **[docs/NEXT_STEPS.md](docs/NEXT_STEPS.md)**.
Как рисовать UI-текстуры под 9-slice: **[docs/UI_SKIN.md](docs/UI_SKIN.md)**.
Система экранов / панелей / диалогов: **[docs/UI_SYSTEM.md](docs/UI_SYSTEM.md)**.
Цветовая палитра (TD × Orcs Must Die): **[docs/UI_PALETTE.md](docs/UI_PALETTE.md)**.
Ниже — самое нужное для старта.

## Сборочные цели

| Цель | Где | Как собрать | Что это |
|---|---|---|---|
| Android-приложение | `app/` | Android Studio (Run ▶) | клиент: рендер GLES3 + игра + сеть |
| Выделенный сервер | `server/` | `server\build.bat` (MSVC) | авторитетный сервер, headless |
| Сервер в Docker | `server/Dockerfile` | `build-server-docker.cmd` | тот же сервер, Linux-контейнер, UDP 7777 |
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

**Docker (сервер):** `build-server-docker.cmd` собирает образ `vbase-server`
(Linux, gcc; не Windows-контейнер), `run-server-docker.cmd` поднимает с
`-p 7777:7777/udp` (или `docker compose up --build`). Клиенты на этой машине —
`127.0.0.1`, телефон — LAN-IP Windows, **не** `172.x` контейнера. Аргументы
пробрасываются в `vbase_server` (`--selftest`, порт, сцена). Нужен Docker Desktop
в режиме Linux containers.

## Тулчейн (эта машина, Windows)

- Android: NDK `28.2.13676358`, GameActivity `3.0.5`, `minSdk 30`, `targetSdk 36`,
  namespace `com.hpg.vbase`. Сборка — только через Android Studio (Gradle wrapper
  не генерировали; из консоли мешает блокировка общего кэша `~/.gradle`).
- Десктоп/сервер: MSVC (VS Build Tools 18, `vcvars64.bat`) + bundled CMake + **Ninja**
  (тоже из поставки VS) — всё зашито в `server/build.bat` и `desktop/build.bat`. Ninja
  собирает параллельно на всех ядрах (NMake был однопоточным, полная сборка Jolt ~3× дольше)
  и точно инкрементит — правка `CMakeLists.txt` больше НЕ пересобирает Jolt. Батник при
  смене генератора сам сносит старую `build/`.
  **Грабля:** комментарии в `.bat` — только ASCII (cmd читает батник в OEM-кодировке, а наши
  файлы сохраняются UTF-8 → кириллица в `REM` станет мусором и «выполнится» как команды).
- Проверка компиляции нативного кода без Gradle: NDK clang с
  `--target=aarch64-linux-android30 -std=c++20 -fsyntax-only` (см. ARCHITECTURE).

## Вендоренные зависимости (`third_party/`)

`imgui` (GUI), `enet` (UDP), `cgltf` (glTF), `stb` (stb_image), `glfw` (окно на десктопе),
`vulkan` (заголовки Khronos, не линкуются), `jolt` (Jolt Physics v5.6.0 — кинематический
контроллер; подключается родным CMake-таргетом, см. NEXT_STEPS §2.5),
`miniaudio` (single-header звук: Windows WASAPI / Android AAudio+OpenSL; за интерфейсом
`engine/audio/Audio.h`, реализация `MiniAudioEngine`; см. NEXT_STEPS «Звук»).
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
- **Недоверенный ввод — валидировать на сервере** (продакшн-цель): сетевые пакеты (величины
  клампятся, направления нормируются, `isfinite`-гарды, поля/индексы в диапазоне), CLI, конфиги
  и ассеты. Клиент может предсказывать, но авторитет и проверки — на сервере; санитизацию,
  общую клиенту и серверу, класть в общий путь (напр. `Character::simulate`), чтобы не расходились.
  Новую проверку покрывать негативным `--selftest`. Список известных дыр/задач — PROJECT_REVIEW.md.
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
