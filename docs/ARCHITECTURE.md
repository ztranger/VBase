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
  - `FollowCamera` — камера 3-го лица за обобщённой целью (позиция+facing).
  - `InputCommand` (в `Input.h`) — ввод за тик (dir + magnitude + faceMove + seq).
    То, что клиент шлёт серверу.
- **Контракт рендера**: `RenderFrame` (`RenderFrame.h`) — камера, свет, список
  `RenderItem` (меш+материал+матрица), список `SkinnedItem` (скиннинг+кости),
  `HudText`, ui-callback (ImGui). Единственное, что игра отдаёт рендеру.
- **Рендер** (`Renderer` интерфейс, `Renderer.h`): `init(window, glGetProc)` +
  `setSurfaceSize` + `createMesh/createSkinnedMesh/createTexture/createMaterial/
  renderFrame/aspectRatio`. Реализация — **`GlRenderer` кроссплатформенный**:
  Android GLES3+EGL и desktop GL 3.3+GLFW под `#ifdef __ANDROID__` (контекст/своп/
  размер платформенные; версия шейдеров — макрос `GLSL_VERSION`; на десктопе
  GL-функции грузит `GlApi.h`). `VulkanRenderer` — второй бэкенд под тем же
  интерфейсом (в сборке обеих целей, десктоп и Android).
- **Геометрия/данные**: `Mesh` (Vertex pos+normal+uv, генераторы plane/cube/sphere),
  `Model` (`SkinnedModel`: вершины со скиннингом, скелет, анимации; sampleAnimation/
  sampleBlend), `Texture` (TextureData, MaterialDesc, ShaderType), `MathUtil`
  (Vec3/Mat4/Quat, perspective/lookAt/slerp/lerpAngle/inverse).
- **Ассеты** (платформонезависимы): `AssetSource` — интерфейс чтения файла;
  `AndroidAssetSource` (в `main.cpp`, поверх AAssetManager), `FileAssetSource`
  (файловая система, десктоп). Загрузчики `Assets` (stb_image, OBJ) и `Model`
  (cgltf) читают через `AssetSource`.
- **Сеть** (`Net.h/.cpp`): `NetClient` / `NetServer` (ENet за pimpl). Протокол —
  POD-структуры (Welcome/Input/Snapshot). Сервер симулирует по `Character` на клиента.

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
  `imgui_impl_glfw`. Содержимое панели — общий модуль `GameUi.{h,cpp}` (imgui+`Scene`),
  один на обе платформы; отдаётся рендеру через колбэк `RenderFrame::ui`.

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

- **Фиксированный тик** 30 Гц (`kTick` в `main.cpp` / десктоп), рендер интерполирует
  между тиками (`alpha`).
- **Свой аватар**: предсказание (локальный `simulate`) + **reconciliation с реплеем**
  — по снапшоту ставим серверное состояние и переигрываем неподтверждённые вводы
  (`Scene::applySnapshot`, буфер `pending_`).
- **Чужие**: **буфер снапшотов + интерполяция** с задержкой ~100 мс
  (`Scene::render`, `TimedState`).
- **Протокол** (`Net.cpp`): `Welcome{entityId}`, `Input{seq,move,mag,face}`,
  `Snapshot{tick,ackSeq,count,EntityState[]}`. POD, `memcpy` (обе стороны ARM/x64,
  little-endian — для кроссплатформы нужна явная сериализация).
- **Host/Join**: телефон может хостить (сервер+клиент к 127.0.0.1) или джойниться;
  либо все джойнятся к выделенному серверу (`server/`). Порт 7777.
- Десктоп-клиент теперь использует общий `Scene`, поэтому получает те же полные
  версии (реплей-реконсиляция + буфер интерполяции). Ввод — WASD через
  `Scene::setMoveInput` (тач-джойстик неактивен).

## 5. Карта файлов

`app/src/main/cpp/` (общее ядро; используется всеми целями):
- Платформонезависимое (игра/данные/сеть): `Character.*`, `Scene.*`, `FollowCamera.h`,
  `Input.h`, `MathUtil.h`, `Mesh.*`, `Model.*`, `Texture.h`, `RenderFrame.h`,
  `Renderer.h`, `Net.*`, `Assets.*`, `AssetSource.h`, `FileAssetSource.h`,
  `SceneDesc.h`/`SceneLoader.*` (описание+парсер сцены), `Log.h` (переносимый).
- Физика (платформонезависима, за интерфейсом): `CollisionWorld.*` — обёртка Jolt
  (pimpl, Jolt не течёт в заголовок), статика арены + кинематические капсулы
  (`CharacterVirtual`). `Character::simulate` двигает через неё; см. NEXT_STEPS §2.5.
- Кроссплатформенный рендер: `GlRenderer.*` (Android GLES3+EGL и desktop GL 3.3+GLFW
  через `#ifdef`), `GlApi.h` (GL: GLES3 на Android / загрузчик на десктопе), `Font.*`.
- Кроссплатформенный GUI: `GameUi.{h,cpp}` — построение ImGui-панели (imgui+`Scene`),
  общее для Android и десктопа; вызывается из `RenderFrame::ui`.
- Кроссплатформенный Vulkan-рендер: `VulkanRenderer.*`, `VkApi.*` (динамический
  загрузчик), `shaders/` (Vulkan GLSL→SPIR-V), `VulkanProbe.*` (проверка доступности).
- Android-специфичное: `main.cpp` (GameActivity, ввод, цикл, EGL/Vulkan-surface из
  окна, `AndroidAssetSource`, переключение бэкендов).
- `CMakeLists.txt` — сборка `libvbase.so`.

`server/`: `main.cpp`, `CMakeLists.txt`, `build.bat` (переиспользует `Net.cpp`,
`Character.cpp` + enet).

`desktop/`: `main.cpp` (GLFW-окно, WASD, цикл), `CMakeLists.txt`, `build.bat`.
Переиспользует из `app/.../cpp` общий `GlRenderer` + `Scene` + `FileAssetSource` +
`Model/Assets/Mesh/Net/Character/Font` + imgui (desktop-режим) + glfw + enet.
(Старые `DesktopRenderer.*`/`GlCore.h` удалены — их заменил кроссплатформенный GlRenderer.)

`third_party/`: `imgui`, `enet`, `cgltf`, `stb`, `glfw`. (`glew/` — пустой, удалить.)

`app/src/main/assets/models/`: `Fox.glb` (CC0, Khronos), `pyramid.obj`.
`app/src/main/assets/shaders/`: GL-шейдеры (`*.vert`/`*.frag` + `common.glsl`),
грузятся рендером через `AssetSource` (см. §3).
`app/src/main/assets/scenes/`: файлы сцен (`*.scene`), грузятся `SceneLoader` (см. §5.1).

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
- `player model <path> [pos x y z] [scale s] [yaw o] [capsule <radius> <cylHalf>]` —
  glTF-персонаж (скиннинг) + капсула кинематического контроллера (по умолчанию 0.3 0.3)
- `light dir <x> <y> <z>` — направление на свет (правится слайдером в `GameUi`)
- `camera [distance d] [height h] [lookHeight l] [fov f] [near n] [far f]`

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
- **Скачивание с GitHub release assets** (GLEW zip, Fox.glb с raw) стабильно
  таймаутит; помогает **jsDelivr** для файлов репозиториев или git clone. GLEW
  не используется — GL-функции на десктопе грузит свой мини-загрузчик `GlApi.h`.
- **INTERNET permission** в манифесте нужен для сокетов.
- Сборка Android из консоли конфликтует с блокировкой `~/.gradle` (Unity/AS-демоны)
  — собирать в Android Studio.

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
4. **Неткод — доведение**: delta-сжатие снапшотов; lag compensation (когда появится
   боёвка/хиты — сейчас неприменима); вынести настройку сервера/адреса.
5. **Геймплей**: мир из многих сущностей (акторы в список; `Character` уже обобщён).
6. Мелочи: своя `crate.png` для текстурированного куба, текстура лисы на десктопе.

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
