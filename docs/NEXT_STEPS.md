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
кейпад для тача. Осталось по желанию: своя `assets/textures/crate.png` для
текстурированного куба.

## Шаг 2 — порт Vulkan-бэкенда под контракт RenderFrame (в работе, фазами)

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
- ✅ **Фаза 6 — Android (написано, проверено только `-fsyntax-only`; на устройстве
  НЕ запускалось).** Ядро (`VulkanRenderer`/`VkApi`/шейдеры/пайплайны) переиспользовано;
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

  **Осталось проверить на устройстве** (Android Studio, отладка по logcat): реальный запуск
  Vulkan-пути, корректность переключения GL↔Vulkan из панели (пересоздание рендера на живом
  ANativeWindow), ImGui-панель под Vulkan на телефоне.

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

## Шаг 3 — доведение неткода

- **Delta-сжатие снапшотов**: слать только изменения от последнего подтверждённого
  снапшота (в протоколе уже есть `serverTick`/`ackSeq`). Актуально при росте числа
  сущностей.
- **Lag compensation**: перемотка чужих на момент действия. **Только когда появится
  боёвка/хиты** — сейчас нечего компенсировать, не делать заранее.
- **Десктоп-неткод до уровня Scene**: сейчас на десктопе упрощённые версии (мягкая
  коррекция + сглаживание). После шага 1 (общий `Scene` на десктопе) он
  автоматически получит полноценные prediction/reconciliation + интерполяцию.
- Конфигурация адреса/порта сервера, переподключение, обработка обрывов.

## Шаг 4 — геймплей и контент

- Мир из многих сущностей: вынести акторов в список (мы к этому вели, `Character`
  уже обобщён — `entityId`, `owner`).
- Коллизии/границы поля, прыжок.
- Текстура лисы на десктопе (уже грузится через stb — включится при рендере).

## Мелкие улучшения (по случаю)

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
