# VBase — архитектура и хендофф

Документ для передачи проекта другому разработчику/AI-агенту. Описывает, что
построено, как устроено и что делать дальше.

## 1. Что это и текущее состояние

Нативный 3D-движок на C++:

- **Android-клиент** (`app/`): GameActivity + OpenGL ES 3. Рендерит сцену
  (пол, кубы, сферы, инстансное кольцо), анимированную glTF-модель (лиса Fox,
  скиннинг), HUD-текст и Dear ImGui. Управление — виртуальный тач-джойстик,
  камера от третьего лица. Работает на устройстве (проверено пользователем).
- **Выделенный сервер** (`server/`): авторитетная симуляция + рассылка снапшотов.
  Собран и запущен на Windows (слушает UDP 7777, печатает свои IP). Проверено.
- **Десктоп-клиент** (`desktop/`): GLFW + desktop GL 3.3, переиспользует общий
  `GlRenderer` (кроссплатформенный) + `Scene` + `FileAssetSource` — рендерит ту же
  сцену и лису (скиннинг/бленд/материалы/HUD/ImGui), что и телефон. Ввод — WASD.
  Собрано и запущено (GL 3.3, GTX 1070), подключается к серверу. Проверено.

Мультиплеер: авторитетный сервер, клиентское предсказание + reconciliation,
интерполяция чужих. ПК↔телефон работают через один сервер.

## 2. Слои и главные абстракции

Данные текут: **ввод → симуляция (фикс. тик) → RenderFrame → рендер**.

- **Игровой слой** (платформонезависим):
  - `Character` — управляемый актор: чистая симуляция (позиция/поворот/скорость/
    параметр анимации). `simulate(dt, InputCommand)` + `snapshot()` (для интерполяции).
    НЕ содержит рендера/модели.
  - `Scene` — мир: окружение (`GameObject`), игрок (`Character`), камера
    (`FollowCamera`), джойстик (`VirtualJoystick`), сеть. `fixedUpdate(dt)` (тик),
    `render(alpha, aspect, dt)` → `RenderFrame` (интерполяция). Строит визуальные
    предметы лисы (`makeFoxItem`). Содержимое сцены не захардкожено: `build()`
    грузит **файл сцены** через `SceneLoader` (см. §5.1) и инстанцирует его.
  - `FollowCamera` — камера ¾-вида: фиксированный наклon (`pitch`), орбитальный `yaw` и зум
    управляются игроком (правый стик / стрелки), следит только за позицией цели. Движение
    героя — camera-relative. (Твин-стик на Android: левый стик — герой, правый — камера.)
  - `InputCommand` (в `Input.h`) — ввод за тик (dir + magnitude + faceMove + seq).
    То, что клиент шлёт серверу.
- **Контракт рендера**: `RenderFrame` (`RenderFrame.h`) — камера, свет, список
  `RenderItem` (меш+материал+матрица), список `SkinnedItem` (скиннинг+кости),
  `HudText`, ui-callback (ImGui). Единственное, что игра отдаёт рендеру.
- **Рендер** (`Renderer` интерфейс, `Renderer.h`): `init(window, glGetProc)` +
  `setSurfaceSize` + `createMesh/createSkinnedMesh/createTexture/createMaterial/
  renderFrame/aspectRatio` + **`getImGuiTexture` / `releaseImGuiTexture`** (ImTextureID
  для пользовательских текстур ImGui). `createTexture(data, clampEdges=false)` —
  при `clampEdges=true` CLAMP_TO_EDGE без mipmaps (UI / 9-slice). Реализация —
  **`GlRenderer` кроссплатформенный**: Android GLES3+EGL и desktop GL 3.3+GLFW под
  `#ifdef __ANDROID__` (контекст/своп/размер платформенные; версия шейдеров — макрос
  `GLSL_VERSION`; на десктопе GL-функции грузит `GlApi.h`). `VulkanRenderer` — второй
  бэкенд под тем же интерфейсом (в сборке обеих целей, десктоп и Android).
- **Геометрия/данные**: `Mesh` (Vertex pos+normal+uv, генераторы plane/cube/sphere),
  `Model` (`SkinnedModel`: вершины со скиннингом, скелет, анимации; sampleAnimation/
  sampleBlend), `Texture` (TextureData, MaterialDesc, ShaderType), `MathUtil`
  (Vec3/Mat4/Quat, perspective/lookAt/slerp/lerpAngle/inverse).
- **Ассеты** (платформонезависимы): `AssetSource` — интерфейс чтения файла;
  `AndroidAssetSource` (в `main.cpp`, поверх AAssetManager), `FileAssetSource`
  (файловая система, десктоп). Загрузчики `Assets` (stb_image, OBJ) и `Model`
  (cgltf) читают через `AssetSource`.
- **Сеть** (`Net.h/.cpp`): `NetClient` / `NetServer` (ENet за pimpl). Протокол —
  POD-структуры (Welcome/Input/Snapshot). Сервер владеет **списком сущностей** (`Entity`):
  герой — сущность типа `Hero` с движением в `Character`; задел под здания/врагов/спавнеры
  (геймплей TD×OMD, см. NEXT_STEPS §4). Снапшот несёт `type`/`team`/`hp`/`aux` на сущность.

## 3. Рендер GlRenderer (что уже есть)

- Материалы + 3 шейдера: **Lit** (диффуз+текстура), **Unlit**, **Phong** (блик).
  Сортировка вызовов по (шейдер, материал, меш) + кэш состояния.
- **UBO** `Frame` (viewProj/lightDir/viewPos) — общий для всех программ, заливается
  раз в кадр (binding 0).
- **Инстансинг**: матрица модели — инстансный атрибут (`iModel`, локации 3–6),
  одинаковые меш+материал рисуются одним `glDrawElementsInstanced`.
- **Скиннинг**: кости в **bone-текстуре** (`RGBA32F`, кость = строка), выборка через
  `texelFetch`; на модель — только `uBoneOffset`. Масштабируется на много моделей.
- **HUD**: свой растровый шрифт 5×7 (`Font.*`), 2D-оверлей; DPI-масштаб.
- **ImGui**: renderer-бэкенд `imgui_impl_opengl3` (общий, в `GlRenderer`); platform-бэкенд
  вне рендера — Android кормит ввод вручную (touch→mouse), десктоп через официальный
  `imgui_impl_glfw`. Содержимое GUI — фасад `GameUi.{h,cpp}` → `engine/render/ui/UiShell`
  (режимы Loading/MainMenu/Battle, hub-панели, floating-окна, DialogStack Ok/YesNo).
  Один код на обе платформы; отдаётся рендеру через колбэк `RenderFrame::ui`.
  Иерархия и чеклист расширения: [UI_SYSTEM.md](UI_SYSTEM.md).
- **UiSkin** (`UiSkin.{h,cpp}`) — тонкий skin поверх ImGui: логика окон/кликов и шрифт
  остаются у ImGui, меняется только отрисовка. `BeginPanel` — окно без стандартного
  фона/title bar + **9-slice** рамка на `DrawList`; `Button` — `InvisibleButton` +
  текстуры normal/hover/active + подпись шрифтом ImGui. Текстуры: `assets/ui/panel.png`,
  `button_normal.png` / `button_hover.png` / `button_active.png` (если нет файлов —
  процедурная генерация). Загрузка: `GameUi::loadSkin` **после** `ImGui_Impl*_Init`
  (Vulkan регистрирует дескрипторы через `ImGui_ImplVulkan_AddTexture`); перед
  Shutdown — `GameUi::unloadSkin` (обязательно на Vulkan). Рамка 9-slice ≈ 25% меньшей
  стороны атласа. Панель: 9-slice + **полоса title** (drag, опциональный крестик при
  `p_open`) + контент в `BeginChild` с inset (`frame` рамки + доп. `pad` внутри).
  Окна **ресайзятся** пользователем (`ConfigWindowsResizeFromEdges`, без
  `AlwaysAutoResize`; на таче увеличен `TouchExtraPadding`). Перегенерация PNG:
  `python app/src/main/assets/ui/gen_ui_skin.py`.
  **Как рисовать PNG под 9-slice** (размеры, 25% border, чеклист) —
  отдельный гайд: [UI_SKIN.md](UI_SKIN.md).
  **Цвета меню/кнопок/мира** (TD × OMD): [UI_PALETTE.md](UI_PALETTE.md).
  **Система экранов/диалогов**: [UI_SYSTEM.md](UI_SYSTEM.md).

Шейдеры GL лежат ассетами в **`app/src/main/assets/shaders/`** (`*.vert`/`*.frag` +
`common.glsl`) и грузятся через `AssetSource` (`GlRenderer::init` его получает).
Файлы содержат чистое тело GLSL: строку версии (`#version 300 es` / `#version 330 core`,
макрос `GLSL_VERSION`) и `precision` для фрагментного подставляет
`assembleShaderSource`; общий UBO-блок `Frame` вынесен в `common.glsl` и
подключается директивой `#include "..."`, которую разворачивает `loadShaderSource`
(1 уровень, ищет рядом с шейдером). Комментарии в шейдер-файлах — ASCII (не-ASCII
даже в комментариях принимают не все GL-драйверы).

NB: не путать с `app/src/main/cpp/shaders/` — там GLSL-исходники **Vulkan**
(`*.vert/.frag`, glslc→SPIR-V в `assets/shaders/vk/`, git-ignored); используются
`VulkanRenderer`.

## 4. Неткод

- **Транспорт vs игра разделены.** `NetServer` (`engine/net/Net.*`) — ЧИСТЫЙ транспорт:
  ENet-события → вызовы `GameWorld`, затем сериализация состояния в дельта-снапшоты.
  Вся авторитетная симуляция (сущности + системы: движение, экономика, спавнеры, враги,
  дальше бой) — в `GameWorld` (`game/GameWorld.*`), БЕЗ ENet. Поэтому геймплей
  тестируется headless (server `--selftest`) и не тонет в сетевом коде. Правило: новые
  игровые системы добавляются в `GameWorld::step`, а не в `NetServer::tick`.
- **Фиксированный тик** `kTickHz` = 30 Гц — единый источник `kTickDt` в `engine/net/Net.h`
  (десктоп-цикл `kTick`, серверный цикл, `Scene::tickDt_` — все ссылаются на него; клиент
  интерполирует чужих по этому шагу, рассинхрон частот = дрейф). Рендер интерполирует
  между тиками (`alpha`).
- **Свой аватар**: предсказание (локальный `simulate`) + **reconciliation с реплеем**
  — по снапшоту ставим серверное состояние и переигрываем неподтверждённые вводы
  (`Scene::applySnapshot`, буфер `pending_`).
- **Чужие**: **буфер снапшотов + интерполяция** с задержкой ~100 мс
  (`Scene::render`, `TimedState`).
- **Протокол** (`Net.cpp`): `Welcome{protocolVersion,entityId}`,
  `Input{seq,move,mag,face,jump,ackTick}`,
  `Snapshot{tick,baseTick,ackSeq,changed[],removed[],phase}` (`phase` = `GamePhase` матча,
  глобально), `Build{buildType,cellX,cellZ}` (запрос постройки клиентом, надёжно). POD,
  `memcpy` (обе стороны ARM/x64, little-endian — для кроссплатформы нужна явная сериализация).
- **Версия протокола** (`kProtocolVersion` в `Net.h`): клиент передаёт её как connect-data,
  сервер сверяет на CONNECT и отклоняет несовпадение ещё ДО спавна героя (+ дублирует в
  Welcome для проверки клиентом). **БАМПАТЬ при любом изменении раскладки сетевых структур**
  (Welcome/Input/Snapshot/`EntityState`) — иначе рассинхрон ABI между отдельно собранными
  билдами Android/десктоп = порча памяти, а не понятная ошибка.
- **Delta-сжатие снапшотов** (Quake3-подход): сервер шлёт изменения относительно
  подтверждённого клиентом тика (`Input.ackTick`), а не полный список. Кольцо истории
  снапшотов (64 тика) на обеих сторонах: сервер — база для дельт, клиент — чтобы применить
  дельту на нужную базу; база выпала → полный снапшот (self-heal). Клиент реконструирует
  полный `states()`, поэтому prediction/interpolation кода не касаются.
- **Host/Join**: телефон может хостить (сервер+клиент к 127.0.0.1) или джойниться;
  либо все джойнятся к выделенному серверу (`server/`). Порт 7777.
- Десктоп-клиент теперь использует общий `Scene`, поэтому получает те же полные
  версии (реплей-реконсиляция + буфер интерполяции). Ввод — WASD через
  `Scene::setMoveInput` (тач-джойстик неактивен).

## 5. Карта файлов

`app/src/main/cpp/` разложен по слоям (include — квалифицированные, напр.
`#include "engine/net/Net.h"`; корень `cpp/` добавлен в include-пути всех трёх целей).
Структура:

```
app/src/main/cpp/
  CMakeLists.txt              сборка libvbase.so
  shaders/                    Vulkan GLSL (→SPIR-V), грузятся рендером через AssetSource
  platform/  main.cpp         Android-точка входа: GameActivity, ввод, цикл, EGL/Vulkan-
                              surface из окна, AndroidAssetSource, переключение бэкендов
  engine/                     переиспользуемый движок
    core/    MathUtil.h Log.h Input.h Camera.h FollowCamera.h Renderer.h RenderFrame.h Texture.h
    render/  GlRenderer.* GlApi.h VkApi.* VulkanRenderer.* VulkanProbe.* GameUi.* UiSkin.*
             ui/  UiShell.* Dialogs.* UiTypes.h screens/ panels/ windows/
    assets/  Assets.* AssetSource.h FileAssetSource.h Model.* Mesh.* Font.*
    physics/ CollisionWorld.*   обёртка Jolt (pimpl, Jolt не течёт в заголовок)
    net/     Net.*              ЧИСТЫЙ транспорт ENet (кормит GameWorld, сериализует снапшоты)
  game/      GameWorld.* Scene.* Character.* SceneDesc.h BuildingConfig.h SceneLoader.* Grid.h
```

Слои по смыслу:
- **engine/core** — платформонезависимая инфраструктура: математика, логи (переносимый
  `Log.h`), ввод, камеры, интерфейсы рендера (`Renderer.h`) и POD-описание кадра
  (`RenderFrame.h`), `Texture.h`. Нейтрально: тянется и симуляцией, и рендером.
- **engine/render** — два бэкенда за интерфейсом `Renderer` + ImGui: `GlRenderer.*`
  (Android GLES3+EGL и desktop GL 3.3+GLFW через `#ifdef`), `GlApi.h`, Vulkan
  (`VulkanRenderer.*`, `VkApi.*` динамический загрузчик, `VulkanProbe.*`), GUI
  (`GameUi.*` — фасад ImGui → `ui/UiShell`; шрифт `loadFont`, skin
  `loadSkin`/`unloadSkin`, см. §3 / [UI_SYSTEM.md](UI_SYSTEM.md)) и skin (`UiSkin.*` —
  9-slice панели + image-кнопки).
- **engine/assets** — загрузка ресурсов: `Assets.*`, `AssetSource.h`/`FileAssetSource.h`,
  `Model.*`, `Mesh.*`, `Font.*`.
- **engine/physics** — `CollisionWorld.*`: статика арены + кинематические капсулы
  (`CharacterVirtual`). `Character::simulate` двигает через неё; см. NEXT_STEPS §2.5.
- **engine/net** — `Net.*`: ЧИСТЫЙ транспорт ENet. `NetServer` кормит `GameWorld` вводом
  (`setHeroInput`), двигает (`step`) и сериализует его состояние в дельта-снапшоты;
  игровой логики в `Net.cpp` больше нет. `NetClient` — приём снапшотов/реконструкция.
- **game** — геймплей: `GameWorld.*` (авторитетная серверная симуляция БЕЗ сети:
  `Entity` + системы движения/экономики/спавнеров/врагов, дальше бой), `Scene.*`
  (клиентский мир, предсказание, реконсиляция, рендер-диспетч), `Character.*`
  (POD-симуляция героя/врага), `SceneDesc.h`/`SceneLoader.*` (описание+парсер сцены),
  `BuildingConfig.h` (типы зданий: параметры+тексты).

Что нужно каждой цели (общий код лежит в `app/.../cpp`, компилируется прямо из него):
- **server** (headless): `engine/net/Net`, `game/GameWorld`, `game/Character`,
  `engine/physics/CollisionWorld`, `game/SceneLoader` + enet.
- **desktop**: то же + `engine/render/*`, `engine/assets/*`, `game/Scene` + imgui/glfw.
- **Android**: всё + `platform/main.cpp`.

`server/`: `main.cpp`, `CMakeLists.txt`, `build.bat`.

`desktop/`: `main.cpp` (GLFW-окно, WASD, цикл), `CMakeLists.txt`, `build.bat`.
(Старые `DesktopRenderer.*`/`GlCore.h` удалены — их заменил кроссплатформенный GlRenderer.)

`third_party/`: `imgui`, `enet`, `cgltf`, `stb`, `glfw`. (`glew/` — пустой, удалить.)

`app/src/main/assets/models/`: `Fox.glb` (CC0, Khronos), `pyramid.obj`.
`app/src/main/assets/shaders/`: GL-шейдеры (`*.vert`/`*.frag` + `common.glsl`),
грузятся рендером через `AssetSource` (см. §3).
`app/src/main/assets/scenes/`: файлы сцен (`*.scene`), грузятся `SceneLoader` (см. §5.1).
`app/src/main/assets/fonts/`: `ui.ttf` — кириллический шрифт ImGui (см. §6).
`app/src/main/assets/ui/`: skin ImGui — `panel.png` (9-slice), `button_*.png`,
скрипт `gen_ui_skin.py`, краткий `README.txt`. Полный гайд по рисованию —
[UI_SKIN.md](UI_SKIN.md).

## 5.1. Формат файла сцены

Текстовый, построчный; `#` — комментарий до конца строки; ссылки (`mat`/`tex`) —
по именам, объявленным выше. Парсер — `SceneLoader` (свой, без зависимостей, в духе
OBJ-загрузчика). Директивы:

- `texture <name> procedural <size> <cells>` | `image <path>`
- `material <name> <lit|unlit|phong> [color r g b] [tex <ref>]` — `ref` = имя текстуры
  или путь к картинке; пусто → без текстуры (белая).
- `mesh <name> plane <size> [uvTiles]` | `cube <size>` | `sphere <r> [stacks] [slices]`
- `object <mesh> mat <mat> [pos x y z] [rot x y z] [scale s] [spin s]`
- `ring <mesh> mat <mat> count <n> radius <r> [y <y>] [scale s] [spin s]` — инстансинг по кольцу
- `collider box center <x y z> half <hx hy hz>` — статичный коллайдер физики (Jolt);
  независим от визуальных мешей (коллизия ≠ отрисовка).
- `generator|storage|spawner|core|tower pos <x y z> [team <n>]` — сущность базы (сервер
  спавнит из неё). В основном РАЗМЕЩЕНИЕ; параметры (rate/cap/hp/damage/range) и тексты —
  в `config/buildings.cfg` (`team` — сторона: 0 соло/кооп, 1/2 PvP).
- `grid cell <размер> arena <полуразмер>` — строительная сетка (клетка + зона стройки).
  Сервер и клиент берут её отсюда (одна геометрия — иначе клиент показал бы валидной клетку,
  которую сервер отвергнет). По умолчанию `cell 2 arena 11`.
- `player model <path> [pos x y z] [scale s] [yaw o] [capsule <radius> <cylHalf>]` —
  glTF-персонаж (скиннинг) + капсула кинематического контроллера (по умолчанию 0.3 0.3)

**Конфиг типов зданий** — `assets/config/buildings.cfg` (`BuildingConfig`, парсер
`loadBuildingConfig`): блок `building <тип>` + `name`/`desc` (весь остаток строки) и
`rate`/`cap`/`interval`/`max`. Единый источник параметров И текстов: и сервер (для
симуляции), и клиент (для инфо-панели по клику) читают его; `applyBuildingConfig`
переносит параметры в здания сцены. Клик/тап по зданию (`Scene::onClick`, raycast) →
панель в `GameUi` с содержимым по типу.
- `light dir <x> <y> <z>` — направление на свет (правится слайдером в `GameUi`)
- `camera [distance d] [pitch p] [lookHeight l] [fov f] [near n] [far f]` — ¾-камера
  (`pitch` — фиксированный наклон, рад; yaw/зум управляются игроком)

Путь к сцене — параметр `Scene::build` (по умолчанию `scenes/default.scene`); на
десктопе задаётся 3-м аргументом: `vbase_desktop.exe [serverIp] [assetsDir] [scenePath]`.
Разные сцены = разные файлы. Ошибка парсинга → лог с номером строки, сцена пустая.

## 6. Грабли (уже решённые — не наступать снова)

- **`project(vbase CXX)` не компилирует `.c`** → для ENet нужен `project(... C CXX)`.
- **`Math.h` ≡ `<math.h>`** на регистронезависимой ФС Windows → назвали `MathUtil.h`.
- **ENet без своего CMake** требует `HAS_*` дефайнов (иначе переопределяет
  `socklen_t`) — заданы в `app/.../CMakeLists.txt` через `set_source_files_properties`;
  на Windows берётся `win32.c` (+ ws2_32/winmm), на Unix — `unix.c` (+ HAS_*).
- **`__builtin_available(android 30,*)`** в NDK 28 НЕ гасит ошибку доступности —
  из-за этого когда-то бампнули `minSdk` до 30 (для `AImageDecoder`). Сейчас
  декодирование на stb_image, так что `minSdk` можно вернуть на 26 при желании.
- **Кириллица в консоли Windows** → `SetConsoleOutputCP(CP_UTF8)` + `/utf-8` (MSVC).
- **Кириллица в ImGui** → встроенный шрифт (ProggyClean) содержит только латиницу,
  русский текст рисуется как `????`. Решение: `assets/fonts/ui.ttf` (DejaVuSans, полная
  кириллица), грузится `GameUi::loadFont` ПОСЛЕ `CreateContext` и ДО инициализации
  бэкенда (сборки атласа), с диапазоном `GetGlyphRangesCyrillic` (латиница+кириллица).
  Байты TTF держатся в `static` (`FontDataOwnedByAtlas=false`) — переживают пересоздание
  контекста при переключении бэкенда. В GlRenderer шрифт грузится в `initGlResources`
  (через член `assets_`), в VulkanRenderer — в `init` (параметр `assets`).
- **UiSkin / кастомные текстуры ImGui.** Шрифт и hit-test остаются у ImGui; внешний вид
  панелей/кнопок — `UiSkin` + PNG в `assets/ui/`. Жизненный цикл: `loadSkin` только
  после `ImGui_Impl*_Init`; на Vulkan `getImGuiTexture` → `ImGui_ImplVulkan_AddTexture`
  (пул: `DescriptorPoolSize` ≥ запаса под font+user textures, сейчас 64); перед
  `ImGui_ImplVulkan_Shutdown` обязательно `unloadSkin` / `releaseImGuiTexture`. На GL
  `ImTextureID` = `GLuint`. UI-текстуры создавать с `createTexture(..., clampEdges=true)`.
  Слайдеры/сепараторы пока стандартные ImGui — при смене арта достаточно заменить PNG
  или править `gen_ui_skin.py`.
- **Скачивание с GitHub release assets** (GLEW zip, Fox.glb с raw) стабильно
  таймаутит; помогает **jsDelivr** для файлов репозиториев или git clone. GLEW
  не используется — GL-функции на десктопе грузит свой мини-загрузчик `GlApi.h`.
- **ImGui `WantCaptureMouse` на тач-вводе отстаёт на кадр.** ImGui пересчитывает
  `WantCaptureMouse` в `NewFrame` (внутри `renderFrame`), а ввод обычно читается ДО
  рендера — значит флаг из прошлого кадра. У мыши это скрыто ховером (позиция известна
  заранее), а у тача ховера нет: первый тап-даун по панели проскакивал проверку и включал
  джойстик лисы. Решение (Android `main.cpp`): `pumpInput` кормит касание в ImGui и
  ЗАПОМИНАЕТ его в `Engine`, а диспатч в геймплей (`onPointer`/`onClick`) — ПОСЛЕ
  `renderFrame`, когда `WantCaptureMouse` уже посчитан по этому касанию. Релиз отдаём
  всегда (чтобы джойстик не залипал). Десктоп не затронут — там мышь наводится до клика.
- **INTERNET permission** в манифесте нужен для сокетов.
- Сборка Android из консоли конфликтует с блокировкой `~/.gradle` (Unity/AS-демоны)
  — собирать в Android Studio.
- **Реплей реконсиляции зовёт `Character::simulate` многократно за тик** → всё, что
  накапливается там (`+= dt`), ускоряется при подключении/хосте (симптом был: анимация
  лисы 2× быстрее онлайн). Поэтому фаза анимации `animTime` крутится НЕ в `simulate`, а
  1 раз/тик в `Scene::fixedUpdate`. Правило: в `simulate` только реконсилируемое состояние
  (сбрасывается снапшотом перед реплеем); чисто косметические/свободные величины — вне его.
- **Состояние, влияющее на симуляцию, ДОЛЖНО реконсилироваться** (обратная сторона того
  же правила). Вертикальная скорость прыжка раньше жила ВНУТРИ `CharacterVirtual` (в
  `CollisionWorld`), реконсиляция сбрасывала позицию (`setCharacterPosition`), но не эту
  скорость → реплей считал прыжок от чужой скорости, прыжок дёргался онлайн. Решение:
  `velocityY` вынесен в `Character` (in/out в `moveCharacter`), шлётся в снапшоте
  (`EntityState.velY`) и сбрасывается на серверную перед реплеем — реплей воспроизводит
  предсказание. (Горизонталь не страдала: скорость считается заново из ввода каждый тик.)
- **`CharacterVirtual` — stateful, телепорт не пересчитывает контакты.** После
  `SetPosition` (реконсиляция) контроллер держит старое ground-state/контакты от
  предсказанной позиции; первый Update реплея считает движение иначе → микро-дрожь
  прыжка каждый снапшот. Решение: `CollisionWorld::setCharacterPosition` после `SetPosition`
  зовёт `RefreshContacts` — пересчитывает контакты на новой позиции, реплей стартует с
  консистентным состоянием. (Вместе с реконсиляцией `velocityY` выше — прыжок гладкий онлайн.)

## 7. Роадмап / что дальше

Ближайшее (по убыванию приоритета):
1. ✅ **Десктопный Renderer-бэкенд** — сделано (GlRenderer кроссплатформенный,
   десктоп рендерит полную сцену/лису через общий `Scene`). Детали — NEXT_STEPS §1.
2. ✅ **Порт Vulkan-бэкенда** под `RenderFrame` — сделано (`VulkanRenderer.*` + загрузчик
   `VkApi.*`). Рисует всё: инстансное окружение (Lit/Unlit/Phong, текстуры), скиннинг
   (кости в SSBO), HUD, ImGui. Бэкенд переключается кнопкой в `GameUi` (Vulkan disabled,
   если недоступен). **Проверен запуском на обеих целях: десктоп и Android** (рендер,
   переключение GL↔Vulkan, ImGui под Vulkan). Детали и фазы — NEXT_STEPS §2.
3. ✅ **Физика — кинематический контроллер на Jolt** — сделано (`CollisionWorld.*`,
   Jolt v5.6.0). Коллизии/границы поля/скольжение по стенам + гравитация/прыжок; клиент
   предсказывает, сервер авторитетно — общий код и геометрия. Проверено на десктопе,
   сервере (`--selftest`) и Android-устройстве. Детали и фазы — NEXT_STEPS §2.5.
4. **Неткод — доведение**: ✅ delta-сжатие снапшотов (сделано, см. §4); осталось —
   lag compensation (когда появится боёвка/хиты — сейчас неприменима); вынести
   настройку сервера/адреса, переподключение.
5. **Геймплей**: мир из многих сущностей (акторы в список; `Character` уже обобщён).
6. Мелочи: своя `crate.png` для текстурированного куба (сейчас отсутствует → куб белый).
   ✅ Skin ImGui (9-slice + кнопки, `UiSkin` + `assets/ui/`) — сделано, см. §3.

Идея-ориентир: всё, что «над рендером» и «над платформой», держать
платформонезависимым; новые фичи вводить как данные в `RenderFrame`/`Scene` и
реализации за интерфейсами, а не хардкодить в конкретный бэкенд.

## 8. Быстрый старт для нового агента

1. Прочитать `CLAUDE.md` (корень) — цели сборки и правила.
2. Android: открыть проект в Android Studio, Run.
3. Сервер: `server\build.bat` → `server\build\vbase_server.exe` (покажет IP).
4. Десктоп: `desktop\build.bat` → `desktop\build\vbase_desktop.exe [serverIp]`.
5. Проверка нативной компиляции без Gradle — NDK clang `-fsyntax-only` (примеры
   команд использовались в истории; таргет `aarch64-linux-android30`, `-std=c++20`,
   include-пути на `third_party/{imgui,cgltf,stb,enet/include}` и prefab GameActivity).
