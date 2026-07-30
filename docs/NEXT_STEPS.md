# VBase — следующие шаги (план работ)

Упорядочено по приоритету, как договаривались. Для каждого шага: цель, что
делать, какие файлы, на что обратить внимание. См. также роадмап в
[ARCHITECTURE.md](ARCHITECTURE.md) §7.

## Шаг 1 — десктопный Renderer-бэкенд ✅ СДЕЛАНО

`GlRenderer` стал **кроссплатформенным** (один бэкенд: Android GLES3 и desktop
GL 3.3). Контекст/своп/размер — платформенные (`#ifdef __ANDROID__` EGL, иначе
GLFW снаружи + `setSurfaceSize`), версия шейдеров через макрос `GLSL_VERSION`
(`300 es` / `330 core`), GL-функции на десктопе грузятся через `GlApi.h` (свой
мини-загрузчик). Интерфейс: `Renderer::init(window, glGetProc)` + `setSurfaceSize`,
`ANativeWindow` — forward-declare. Десктоп-клиент теперь использует общий `Scene`
+ `FileAssetSource` и рендерит ту же сцену/лису (скиннинг/бленд/материалы/HUD/ImGui).
Собрано и запущено (NVIDIA GTX 1070, GL 3.3.0), подключается к серверу.

Ввод в ImGui на десктопе ✅ сделано: подключён официальный platform-бэкенд
`imgui_impl_glfw` (мышь/клавиатура/скролл/курсоры). Renderer-бэкенд
`imgui_impl_opengl3` остался в `GlRenderer` (общий), platform-бэкенд — за его
пределами: Android кормит ввод вручную (touch→mouse), десктоп — через glfw.
Само содержимое панели вынесено в общий модуль `GameUi.{h,cpp}` (тянет только
imgui + `Scene`, не ломает платформонезависимость ядра) и одинаково на телефоне
и ПК. WASD гейтится по `io.WantCaptureKeyboard`, поле IP — `InputText` + свой
кейпад для тача. Внешний вид панелей/кнопок — `UiSkin` + `assets/ui/` (см.
ARCHITECTURE §3). Осталось по желанию: своя `assets/textures/crate.png` для
текстурированного куба.

## Шаг 2 — порт Vulkan-бэкенда под контракт RenderFrame ✅ СДЕЛАНО (десктоп + Android)

**Цель:** второй бэкенд под тем же `Renderer`/`RenderFrame`. Делаем **десктоп-first,
кроссплатформенно**: разрабатываем и запускаем на десктопе (GLFW+Vulkan, флаг
`--vk`), ядро потом переиспользуем на Android. Причина — Vulkan без запуска почти
не заводится, а Android здесь не запускается; на десктопе итерируем.

**Тулчейн:** заголовки Khronos вендорены в `third_party/vulkan/`; функции грузятся
динамически через `glfwGetInstanceProcAddress` (загрузчик `VkApi.*`) — vulkan-1.lib
для запуска не нужен. Шейдеры GLSL→SPIR-V — через `glslc` (SDK:
`C:\VulkanSDK\1.4.350.0\Bin\glslc.exe`, либо NDK `shader-tools/`).
**Validation layers:** установлен Vulkan SDK (`C:\VulkanSDK\1.4.350.0`), слои
`VK_LAYER_KHRONOS_validation` включаются автоматически при наличии + debug messenger
шлёт сообщения в лог. Фаза 0 проходит валидацию чисто.

**Фазы:**
- ✅ **Фаза 0** — instance/device/swapchain/render pass/present, очистка экрана
  (`VulkanRenderer.cpp`, `VkApi.*`). Собрано и запущено на десктопе (`--vk`).
- ✅ **Фаза 1** — пайплайн Lit + вершинные/индексные буферы (host-visible) + **depth** +
  per-object матрица (push-константа) + frame-UBO (viewProj/свет, дескриптор-сет на кадр)
  + коррекция клип-пространства GL→Vulkan (`vulkanClipFix`). Шейдеры `cpp/shaders/lit.*`
  → SPIR-V через `glslc` (CMake), грузятся из `assets/shaders/vk/`. Окружение рисуется,
  валидация чиста. Cull=NONE (winding — потом). Материалы пока только цвет (текстуры — Ф2).
- ✅ **Фаза 2** — текстуры (staging → device-local image, барьеры layout, сэмплер
  LINEAR+REPEAT, дефолтная белая 1x1) + три пайплайна Lit/Unlit/Phong (общий
  pipeline layout: set 0 = Frame UBO, set 1 = albedo материала, push = model+color).
  Шахматка на полу и блики Phong рисуются, валидация (вкл. synchronization) чиста.
- ✅ **Фаза 3** — инстансинг. Модельная матрица — per-instance атрибут `iModel`
  (binding 1, локации 3–6) вместо push-константы; push теперь только цвет (фрагментный).
  `renderFrame` батчит `frame.items` по (mesh, material), раскладывает матрицы в
  per-frame инстанс-буфер (host-visible, `kMaxInstances`=512) и рисует каждый батч
  одним `vkCmdDrawIndexed(instanceCount, firstInstance=...)`. Кольцо 48 кубов = 1 draw
  (всего ~8 draw вместо 55). Валидация чиста.
- ✅ **Фаза 4** — скиннинг (лиса). Кости — в per-frame **SSBO** (set 2, `mat4 bones[]`,
  `kMaxBones`=512), смещение модели в `uBoneOffset` (push). Отдельный skinned-пайплайн
  и layout (set0 Frame + set1 albedo + set2 bones; push = model+color+boneOffset,
  VERTEX|FRAGMENT); vertex-формат `SkinnedVertex` (16 float). `renderFrame` раскладывает
  кости всех `frame.skinned` в SSBO и рисует каждый объект своим `uBoneOffset`. Текстуры
  скиннинга — свои set 1 (`textureSets_` + `whiteSet_`). Лиса рисуется, валидация чиста.
- ✅ **Фаза 5** — HUD + ImGui. HUD: атлас `Font` → текстура (NEAREST/CLAMP-сэмплер),
  свой пайплайн (alpha-blend, без depth), per-frame динамический буфер квадов, push =
  screen+color; NDC без Y-флипа (Vulkan Y вниз). ImGui: `imgui_impl_vulkan` c
  `IMGUI_IMPL_VULKAN_NO_PROTOTYPES` + `ImGui_ImplVulkan_LoadFunctions` (наш загрузчик
  `vkApiLoader`), `DescriptorPoolSize>0` (пул создаёт бэкенд), `PipelineInfoMain.RenderPass`;
  контекст создаёт `VulkanRenderer`, platform-бэкенд `ImGui_ImplGlfw_InitForVulkan` — в main.
  Панель `GameUi` та же, что на GL/Android. Полный паритет с GL, валидация чиста.
- ✅ **Рантайм-переключение бэкенда (десктоп)** — кнопки OpenGL/Vulkan в панели
  `GameUi` (`GameUiState.backend`/`requestBackend`/`vulkanAvailable`; кнопка текущего
  и недоступного бэкенда — disabled, доступность по `glfwVulkanSupported`). Десктоп-main
  унифицирован в `runClient(backend)`: создаёт своё окно (у GL и Vulkan разный
  GLFW client-API) + рендер + сцену, по нажатию возвращает следующий бэкенд, `main`
  перезапускает. Проверено (переключение GL↔Vulkan в рантайме работает).
- ✅ **Фаза 6 — Android (проверено на устройстве: работает корректно).** Ядро
  (`VulkanRenderer`/`VkApi`/шейдеры/пайплайны) переиспользовано;
  отличия под `#ifdef __ANDROID__`:
  - surface — `vkCreateAndroidSurfaceKHR` (+ расширение `VK_KHR_android_surface`);
  - bootstrap загрузчика — `dlopen("libvulkan.so")` + `dlsym("vkGetInstanceProcAddr")`;
  - `VkApi.h` — на Android `#define VK_USE_PLATFORM_ANDROID_KHR` до `vulkan.h`, surface-функция
    в отдельном `VK_PLATFORM_INSTANCE_FUNCS`;
  - ImGui — без `imgui_impl_glfw`: ввод в IO кормит Android `main.cpp` (touch→mouse),
    `DisplaySize/DeltaTime` в `VulkanRenderer::renderFrame` под `#ifdef __ANDROID__`.
  - **Готча линковки решена:** `libvulkan` УБРАН из линковки (`dl` вместо него),
    `VulkanProbe` переведён на dlopen-загрузчик — иначе указатели `VkApi` конфликтуют с
    экспортами libvulkan (duplicate symbol).
  - `app/.../CMakeLists.txt`: добавлены `VulkanRenderer.cpp`/`VkApi.cpp`/`imgui_impl_vulkan.cpp`
    (+ `IMGUI_IMPL_VULKAN_NO_PROTOTYPES`, вендоренные заголовки Vulkan); в Android `main.cpp` —
    `createRenderer(backend)` + переключение по `GameUiState.requestBackend` (тот же путь, что на десктопе).

  **Проверено на устройстве:** запуск Vulkan-пути, переключение GL↔Vulkan из панели
  (пересоздание рендера на живом ANativeWindow) и ImGui-панель под Vulkan — работают.

**⚠️ Сборка (важно):** инкрементальный NMake-билд НЕ пересобирает `desktop/main.cpp`
при изменении заголовка `VulkanRenderer.h`. Т.к. `main.cpp` создаёт `VulkanRenderer`
на стеке, устаревший layout объекта = запись за его границы = порча памяти (симптом:
зависание/краш на старте, `frames_` с мусорным размером). **После правок заголовков
делать чистую пересборку** (`rm -rf desktop/build` перед `build.bat`). Диагностику
ловит synchronization validation (включена в `createInstance` при наличии SDK).

**Дальше по контракту (в рамках фаз 1–5):**
- Переписать под `Renderer`/`RenderFrame`:
  меши/текстуры/материалы/скиннинг, per-object матрицы, **depth-буфер**,
  пересоздание swapchain при ресайзе.
- Вернуть компиляцию шейдеров glslc→SPIR-V (в `app/.../CMakeLists.txt` был блок,
  сейчас убран; `shaders/*.vert|frag` на месте).
- Точка выбора бэкенда уже заложена в `main.cpp` (`engine.vulkanSupported`).

## Шаг 2.5 — физика: кинематический контроллер на Jolt ✅ СДЕЛАНО (десктоп + сервер + Android)

**Цель:** коллизии/границы/прыжок для персонажа. Только **кинематика** (Jolt
`CharacterVirtual`), динамика твёрдых тел не нужна. Движок — **Jolt Physics**
(вендорен в `third_party/jolt`, тег **v5.6.0**).

**Архитектура (важно — диктуется мультиплеером):**
- Движение живёт в `Character::simulate`, вызывается в трёх местах: локальное
  предсказание, реплей реконсиляции ([Scene.cpp](../app/src/main/cpp/Scene.cpp)) и
  сервер ([Net.cpp](../app/src/main/cpp/Net.cpp)). Коллизии должны считаться **тем же
  кодом** на клиенте и сервере — иначе предсказание у стены будет «резинить».
- План: `CollisionWorld` (обёртка Jolt, платформонезависима, рядом с `Character`/
  `Scene`/`Net`) держит статику арены и `CharacterVirtual` на игрока (по хэндлу).
  `Character::simulate(dt, in, CollisionWorld*)` двигает через мир (collide-and-slide),
  а не `position +=`; `Character` остаётся POD (состояние контроллера — в мире).
- **Сервер сейчас безмировой** (знает только позиции) — для коллизий ему нужна та же
  геометрия: расширить `.scene` директивами `collider ...`, сервер грузит тот же файл
  через `FileAssetSource`.

**Подключение Jolt (готчи — соблюдать):**
- Через **родной CMake-таргет** `Jolt` (`add_subdirectory(third_party/jolt/Build ...)`
  + `target_link_libraries(<цель> Jolt)`). Все `JPH_*`-дефайны и include у таргета
  **PUBLIC** → у либы и потребителя совпадают автоматически (рассинхрон = порча памяти).
  Руками дефайны/пути НЕ прописывать.
- Опции перед `add_subdirectory` (во всех трёх целях): `JPH_USE_DX12/VK/MTL OFF`
  (GPU-compute не нужен и тянет dxc/glslc; на Android VK по умолчанию ON — обязательно
  выключить), `ENABLE_ALL_WARNINGS OFF` (иначе `/WX` ломает сборку на новом MSVC),
  `INTERPROCEDURAL_OPTIMIZATION OFF` (LTO мешает миксу со static; на ARM ломает кодоген),
  `ENABLE_INSTALL OFF`; на MSVC — `USE_STATIC_MSVC_RUNTIME_LIBRARY OFF` (/MD как у нас).
  CPU-compute оставляем (чистый C++, нужен Hair — он в исходниках безусловно).
- **`CMAKE_BUILD_TYPE` обязателен** — у `build.bat` был пуст, Jolt без конфига не
  собирается: в `desktop/build.bat` и `server/build.bat` добавлен `-DCMAKE_BUILD_TYPE=Release`.
- В коде: `#include <Jolt/Jolt.h>` ПЕРВЫМ; `using namespace JPH::literals;` для суффикса `_r`.

**Фазы:**
- ✅ **Фаза 0 — сборка на всех трёх целях + smoke.** Проверено на устройстве/десктопе/
  сервере (Jolt собирается и линкуется). Временный `PhysicsSmoke` уже удалён — заменён
  `CollisionWorld`.
- ✅ **Фаза 1+2 — `CollisionWorld` + статика из сцены + контроллер игрока (клиент, офлайн).**
  Сделаны одним заходом (Фаза 1 сама по себе без видимого результата).
  - `CollisionWorld.{h,cpp}` — обёртка Jolt (pimpl, Jolt НЕ течёт в ядро): статичные
    боксы + кинематические капсулы (`CharacterVirtual`, `Update` без гравитации).
  - Формат сцены расширен: `collider box center <x y z> half <hx hy hz>` и у `player`
    ключ `capsule <radius> <cylHalf>` (по умолчанию 0.3 0.3). `SceneDesc`/`SceneLoader`.
  - `Character::simulate(dt, in, CollisionWorld*)` двигает через `moveCharacter`
    (collide-and-slide), fallback без мира — прямое интегрирование. `Character` — POD,
    хэндл контроллера в поле `collider`; состояние капсулы живёт в мире.
  - `Scene` владеет `CollisionWorld` (unique_ptr, `~Scene` в .cpp), строит коллайдеры
    и контроллер в `build`, прокидывает мир в предсказание и в реплей реконсиляции
    (перед реплеем `setCharacterPosition` — синхронизация с сервером, задел под Фазу 3).
  - `default.scene`: пол + 4 стены-границы (±12) + 3 куба-препятствия (8 коллайдеров).
  - **Проверено:** headless-самотест `vbase_server --selftest` — лобовое (x упирается
    ~1.43 вместо 5) и скольжение вдоль стены (z проезжает −2→5.5) → `OK`; десктоп
    стартует с физикой без краша, `Физика: 8 статичных коллайдеров`. Визуальный проход
    WASD по десктопу — за пользователем (инъекция клавиш в окно из автоматизации ненадёжна).
    Капсула — в игровом масштабе, не в масштабе крошечной лисы (scale 0.03); тюнится
    ключом `capsule` в сцене.
- ✅ **Фаза 3 — сервер: та же геометрия + контроллеры.**
  - `NetServer::configureWorld(const SceneDesc&)` строит внутренний `CollisionWorld` из
    тех же коллайдеров (+ спавн/капсула из `player`). Вызывать после `start`; без него —
    fallback без коллизий. `NetServer::Impl` владеет миром.
  - На подключение клиента — спавн `CharacterVirtual` в мире сервера (позиция + хэндл
    в `ServerClient.ch.collider`); на отключение — `removeCharacter`; `tick` симулирует
    через `c.ch.simulate(dt, in, world)` — тот же код, что предсказывает клиент.
  - Dedicated-сервер грузит сцену сам: `FileAssetSource` + `loadSceneDesc`, аргументы
    `vbase_server [port] [assetsDir] [scenePath]` (в сборку добавлен `SceneLoader.cpp`).
    Host-режим (`Scene::hostGame`) отдаёт серверу сохранённый `sceneDesc_` — та же геометрия.
  - Реконсиляция у стены: клиент и сервер считают одним кодом по одной геометрии →
    расхождений почти нет; `applySnapshot` перед реплеем синхронит контроллер
    (`setCharacterPosition`, сделано в Ф2).
  - **Проверено:** `vbase_server --selftest` теперь гоняет два теста — примитив
    collide-and-slide И серверный путь по loopback (сетевой клиент шлёт ввод в стену →
    авторитетный x=1.43, `myId=1`, `OK`). Dedicated-сервер логирует `мир коллизий —
    8 статичных коллайдеров`. Десктоп пересобран, без регрессий. `CollisionWorld.cpp`
    под aarch64 — `-fsyntax-only` чисто.
- ✅ **Фаза 4 — прыжок + гравитация.**
  - Вертикаль ведёт `CollisionWorld::moveCharacter(id, horizVel, jump, dt)` — вызывается
    КАЖДЫЙ тик (гравитация/земля/прыжок работают и стоя). Внутри: читаем вертикальную
    скорость контроллера, на земле гасим, каждый тик `-= g*dt`, прыжок с земли `= vJump`;
    `Update` с реальной гравитацией. Константы: `kGravity=18`, `kJumpSpeed=6` (высота ~1).
  - `Character::simulate` прогоняет контроллер безусловно (не только при движении).
  - `InputCommand.jump` + бит в протоколе (`InputMsg.jump`): прыжок шлётся **надёжно**
    (ENet reliable — одноразовое событие не терять) и **гасится на сервере** после
    применения (не «залипал» между пакетами).
  - Ввод прыжка: десктоп — пробел по фронту (`Scene::requestJump`), тач/Android — кнопка
    «Jump» в `GameUi`. `Scene` копит одноразовый `jumpQueued_`, выдаёт в `cmd.jump`.
  - **Готча:** у свежего `CharacterVirtual` ground-state = InAir до первого `Update`,
    поэтому прыжок на самом первом тике игнорируется (в игре неактуально — персонаж уже
    на земле; в самотесте — «прогрев» несколькими тиками).
  - **Проверено:** `--selftest` — прыжок `apex=1.000` (ровно `v²/2g`), приземление `y≈0`;
    лоб/слайд без регрессий; серверный путь `OK`. Десктоп пересобран без регрессий;
    `CollisionWorld.cpp`/`Character.cpp` под aarch64 — чисто.
- ✅ **Фаза 5 — проверка контроллера на Android-устройстве.** Собрано в Android Studio,
  запущено на девайсе: коллизии/границы/прыжок (кнопка Jump в `GameUi`) работают корректно.

**Кросс-платформенный детерминизм** (ARM↔x64): для предсказания бит-в-бит НЕ обязателен
(мелкие расхождения гасит реконсиляция). Если позже понадобится — опция Jolt
`CROSS_PLATFORM_DETERMINISTIC ON` (+ fast-math off), включать при появлении предсказания.

## Шаг 3 — доведение неткода

- ✅ **Delta-сжатие снапшотов** — сделано (Quake3-подход, весь в `Net.cpp`, `Scene` не
  тронут). Клиент подтверждает последний применённый тик (`InputMsg.ackTick`); сервер
  шлёт дельту относительно ПОДТВЕРЖДЁННОГО тика (устойчиво к потерям UDP), а не последнего
  посланного. `SnapshotHeader`: `baseTick` (0 = полный) + `changedCount`/`removedCount`,
  далее изменившиеся `EntityState` и id исчезнувших. Кольцо истории снапшотов (64 тика ≈ 2 c)
  на **обеих** сторонах: сервер — база для дельт, клиент — чтобы применить дельту на нужную
  базу (не на «текущее», иначе рассинхрон при потере ack). База выпала из кольца →
  сервер шлёт полный (self-heal). «Изменилось» — по эпсилону 1e-3 (гасит асимптотику
  speed01/animParam). Клиент реконструирует полный `states()` — поэтому `Scene`/реконсиляция
  без изменений. **Проверено:** `--selftest` — реконструкция верна (авторитетный x=1.43),
  в простое дельта схлопывается (`changed=0`); десктоп без регрессий; `Net.cpp` под aarch64 чисто.
- **Lag compensation**: перемотка чужих на момент действия. **Только когда появится
  боёвка/хиты** — сейчас нечего компенсировать, не делать заранее.
- **Десктоп-неткод до уровня Scene**: сейчас на десктопе упрощённые версии (мягкая
  коррекция + сглаживание). После шага 1 (общий `Scene` на десктопе) он
  автоматически получит полноценные prediction/reconciliation + интерполяцию.
- Конфигурация адреса/порта сервера, переподключение, обработка обрывов.

## Шаг 4 — геймплей: TD × Orcs Must Die (фазами)

**Вижн:** герой строит базу (генераторы → ресурс капает в хранилища с потолком),
спавнеры по правилам плодят врагов, те бегут на базу/игрока. Режимы: **соло**,
**кооп** (2–4 строят одну базу), **PvP** (1v1/2v2: каждая сторона строит оборону +
наступательные спавнеры на врага). Строим соло-first — кооп/PvP это тот же
авторитетный сим + команды/владение поверх готовой петли.

**Фазы:**
- ✅ **G0 — общая система сущностей (фундамент).** Сервер владеет списком `Entity`
  (не «один `Character` на клиента»): `NetServer::Impl.entities` + `conns` (подключения).
  Герой — сущность типа `Hero` с `Character` в `move` (тот же код предсказания, что у
  клиента). `EntityState` обобщён: `type`/`team` + generic-слоты `hp`/`aux`
  (ресурс/прогресс/…). Снапшот строится из всех сущностей; дельта учитывает новые поля.
  Клиент/`Scene` пока не тронуты (все сущности — герои, рендер как раньше). **Проверено:**
  `--selftest` без регрессий (авторитетный x=1.43, дельта в простое 0); десктоп без краша;
  `Net.cpp` под aarch64 чисто. Типы `EntityType` (Generator/Storage/Spawner/Enemy/Tower/
  Core) заведены как задел.
- ✅ **G1 — здания + экономика.**
  - Формат сцены: `generator pos <x y z> rate <r>` и `storage pos <x y z> cap <c>`
    (`SceneDesc.buildings`, парсер в `SceneLoader`). Сервер спавнит из них сущности
    `Generator`/`Storage` в `configureWorld`.
  - Экономика в `NetServer::tick`: генераторы капают в общий пул (`Impl.resource`, team 0),
    ограниченный суммой ёмкостей хранилищ (лишнее теряется = потолок накоплений); пул
    раскладывается по хранилищам в поле `aux` — так попадает в сеть. Параметры зданий
    (`rate`/`cap`) — на серверной `Entity`, не в сети.
  - Клиент: `RemotePlayer` → типизированная `RemoteEntity` (все чужие сущности), рендер
    **по типу** (`Scene::render` switch: Hero→лиса, Generator→зелёный куб, Storage→синий).
    `remoteCount()` считает только героев. HUD в `GameUi` — «Resource: тек / потолок»
    (`Scene::resourceCurrent()` = сумма `aux` хранилищ из снапшотов; `resourceCap()` =
    сумма `cap` из `sceneDesc_`).
  - **Проверено:** `--selftest` `EconTest` — сервер капит пул в 25 И клиент по сети
    получает 1 хранилище с `ресурс=25` (весь путь сервер→снапшот→клиент). Десктоп без
    регрессий; `Net.cpp`/`Scene.cpp`/`SceneLoader.cpp` под aarch64 чисто. Визуал (кубы+HUD)
    виден при подключении к серверу (соло-экономика идёт только на сервере/в host-режиме).
  - Здания пока БЕЗ коллизий (через них можно ходить) — добавим на G3 (футпринты/размещение).
- ✅ **G2 — спавнеры + враги.**
  - Формат сцены: `spawner pos <x y z> interval <s> max <n>` и `core pos <x y z>`
    (расширен `BuildingSpec.kind`: +Spawner/Core; `rate`=интервал, `cap`=макс). Сервер
    спавнит `Spawner`/`Core` как сущности в `configureWorld`.
  - Сервер `tick`: спавнеры по таймеру (`rate`) плодят `Enemy` до потолка (`cap`, поле
    `spawnedCount`) — новые сущности копятся в temp-векторе и добавляются ПОСЛЕ цикла
    (иначе `push_back` инвалидирует итерацию). Враги бегут к первому `Core` тем же
    `moveCharacter` (гравитация + скольжение по стенам), останавливаясь в `kCoreStopDist`;
    `kEnemySpeed=3`. Боя пока нет.
  - Клиент рендерит по типу: враг — красная сфера, спавнер — фиолетовый куб, ядро —
    золотая сфера (Phong). Всё стримится как обычные сущности (интерполяция из буфера).
  - **Проверено:** `--selftest` `SpawnTest` — спавнер породил 3 врага (потолок), клиент
    по сети видит все 3, ближайший добежал до ядра (дист 0.9 от старта 8). Десктоп без
    регрессий; `Net.cpp`/`Scene.cpp`/`SceneLoader.cpp` под aarch64 чисто.
  - **Ограничение:** движение — чистый steer-to-target (без A*), враги скользят по стенам,
    но могут «залипнуть» за прямым препятствием между ними и ядром. Настоящий пасфайндинг —
    когда понадобится (сейчас пути в сцене подобраны свободными).
- ✅ **Доводка — конфиг зданий + инфо-панель по клику.**
  - Параметры И тексты типов — в `assets/config/buildings.cfg` (`BuildingConfig`,
    блок `building <тип>` + `name`/`desc`/`rate`/`cap`/`interval`/`max`). Сцена теперь
    только РАЗМЕЩАЕТ (`generator/storage/spawner/core pos <x y z>`), параметры берёт из
    конфига (`applyBuildingConfig`). И сервер, и клиент читают конфиг (единый источник).
  - Клик мышью (десктоп) / тап (Android) → `Scene::onClick`: unproject экран→мировой луч
    (inverse(proj·view), вручную т.к. в MathUtil нет Vec4) + ray-sphere по зданиям →
    `selectedId_`. Клик мимо снимает выделение.
  - Панель в `GameUi` (стабильный `###`- id): имя+описание из конфига + типовые параметры
    (генератор — генерация/сек; хранилище — ёмкость и текущее; спавнер — интервал/максимум).
  - Проверено: сервер грузит сцену+конфиг (4 сущности базы), самотесты без регрессий,
    десктоп без краша, `Scene.cpp`/`SceneLoader.cpp`/`Net.cpp` под aarch64 чисто. Визуал
    (клик→панель) — на подключённой сессии/host; Android-ввод — на устройстве.
- **G3 — бой + размещение героем.** Мобы бьют ядро/героя; защита (башня/ловушка/атака
  героя) бьёт мобов; hp/смерть/очистка. Здания ставит герой (build-команда) за ресурс.
  **Тогда же выбрать модель размещения** (сетка vs свободно). Играбельная соло-оборона.
- **G4 — кооп** (общая база: команды/владение, общий ресурс).
- **G5 — PvP** (две базы, наступательные спавнеры, условие победы).

## Мелкие улучшения (по случаю)

- ✅ **ImGui skin (9-slice + кнопки)** — сделано. Модуль `UiSkin.{h,cpp}`: `BeginPanel` /
  `Button` поверх логики ImGui; PNG в `assets/ui/` (+ `gen_ui_skin.py`); `Renderer::
  getImGuiTexture` / `createTexture(..., clampEdges)`; `GameUi::loadSkin`/`unloadSkin`.
  Слайдеры и прочие виджеты пока дефолтные. См. ARCHITECTURE §3 (UiSkin) и §6.
- **Фазовая синхронизация бленда** Walk↔Run (сейчас обе анимации от одного времени,
  лапы могут слегка рассинхрониться — вести нормализованную фазу и умножать на
  длительность каждой анимации).
- **Событийные анимации** (тап/кнопка → Run с авто-блендом за ~0.2с).
- Frustum culling, атласы текстур — когда сцена разрастётся.
- Настоящий текстовый IME (для чата/имён) через `GameActivity_showSoftInput` +
  подача символов в ImGui. Для IP хватает встроенной цифровой клавиатуры.

## Принцип

Всё «над рендером» и «над платформой» держим платформонезависимым. Новые фичи —
как данные в `RenderFrame`/`Scene` и реализации за интерфейсами (`Renderer`,
`AssetSource`), а не хардкодом в конкретный бэкенд.
