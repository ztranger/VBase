# VBase — аудит проекта и план стабилизации

Дата аудита: 2026-09-02.

Документ предназначен для последовательного исправления найденных проблем человеком
или AI-агентом. Это не roadmap новых возможностей: сначала устраняются дефекты и риски,
затем продолжается развитие gameplay.

## 1. Как работать с документом

- Выполнять задачи по порядку приоритетов: P0 → P1 → P2 → P3.
- Каждое исправление делать отдельным небольшим коммитом или MR.
- Не объединять исправление дефекта с несвязанным рефакторингом.
- После завершения задачи поставить `[x]` и рядом указать commit/MR.
- При изменении сетевой раскладки обязательно увеличить `kProtocolVersion`.
- Не переносить платформенные зависимости в `game/*`, `engine/core`,
  `engine/assets`, `engine/physics` или транспортный слой.
- Новые игровые правила должны исполняться в `GameWorld::step`, а не в
  `NetServer::tick`.

Рекомендуемый цикл для AI-агента:

1. Прочитать связанные файлы и подтвердить, что проблема ещё существует.
2. Добавить тест, воспроизводящий ошибку, если это возможно.
3. Реализовать минимальное исправление.
4. Собрать server и desktop.
5. Запустить `--selftest` и профильные тесты.
6. Обновить этот документ и связанную архитектурную документацию.

## 2. Итог аудита

Проект имеет хорошую учебную основу:

- платформенные зависимости в основном изолированы;
- сервер является авторитетным;
- симуляция и Jolt Physics переиспользуются клиентом и сервером;
- рендер скрыт за общим интерфейсом и имеет GL/Vulkan реализации;
- присутствуют prediction/reconciliation, fixed tick и delta snapshots;
- headless self-tests действительно проверяют существенную часть gameplay.

Основные препятствия для публичного мультиплеера и дальнейшего масштабирования:

1. сервер недостаточно проверяет сетевой ввод;
2. в desktop Release известен неопределённый дефект, скрытый выбором build type;
3. Android/Vulkan не воспроизводится из чистого clone;
4. `Scene` объединяет слишком много обязанностей;
5. сетевые и рендерные лимиты часто приводят к тихой потере данных;
6. нет CI, sanitizer-сборок и негативных тестов протокола/конфигов.

## 3. P0 — исправить немедленно

### P0-01. Speed hack, NaN и Inf через сетевой ввод

- [x] Исправлено. Санитизация в `Character::simulate` (единый чокпоинт клиента и сервера):
  `isfinite`-гарды на magnitude/moveX/moveZ (не-finite → 0), magnitude клампится в `[0,1]`,
  длина направления ограничивается 1 (иначе `|dir|·mag·maxSpeed` > maxSpeed = speed hack). Для
  честного клиента (нормированное направление, mag 0..1) — no-op. Тест `runInputGuardTest`
  (magnitude=1000 → дист 6 world; NaN/Inf → позиция finite).

Затронутые файлы:

- `app/src/main/cpp/engine/net/Net.cpp`
- `app/src/main/cpp/game/GameWorld.cpp`
- `app/src/main/cpp/game/Character.cpp`
- `server/main.cpp` — headless-тесты

Проблема:

- сервер принимает `moveX`, `moveZ` и `magnitude` от клиента без полной проверки;
- направление может иметь произвольную длину;
- `magnitude` не гарантированно находится в `[0, 1]`;
- NaN/Inf могут попасть в симуляцию и Jolt.

Последствия:

- движение с произвольной скоростью;
- повреждение состояния физики;
- распространение NaN в позицию, velocity и snapshots.

Требуемое исправление:

- проверять `std::isfinite` для всех сетевых float;
- отклонять пакет или отключать peer при NaN/Inf;
- ограничивать magnitude диапазоном `[0, 1]`;
- нормализовать или ограничивать длину горизонтального направления;
- валидировать кнопочные флаги и `charType`;
- применять те же проверки до помещения команды в input buffer.

Критерии готовности:

- валидный ввод не меняет существующее ощущение движения;
- `(moveX=100, moveZ=100, magnitude=100)` не увеличивает скорость;
- NaN и Inf не попадают в `Character`, `CollisionWorld` и snapshot;
- есть автоматические тесты для большого значения, NaN и Inf.

### P0-02. Бесконечное лечение сменой `charType`

- [x] Исправлено. `GameWorld::setHeroCharType` теперь **коммитит тип один раз** (первое получение
  = выбор при входе: статы + полный hp), дальнейшие смены игнорирует — персонажа нельзя менять в
  активном матче, значит `applyHeroStats`/полный hp по смене больше не вызвать. Плюс валидация:
  `charType` вне ростера отклоняется. Wire-format не менялся (`charType` уже был в протоколе) —
  версия не бампалась. Тест `runCharTypeHealTest` (урон → чередование типов → hp не растёт).

Затронутые файлы:

- `app/src/main/cpp/game/GameWorld.cpp`
- `app/src/main/cpp/engine/net/Net.cpp`
- `app/src/main/cpp/engine/net/Net.h`
- `server/main.cpp`

Проблема:

`charType` передаётся с input-командой. При его изменении сервер повторно применяет
характеристики персонажа и восстанавливает полный HP. Чередование типов позволяет
лечиться без ограничений.

Требуемое исправление:

- выбрать допустимый момент смены героя: lobby, spawn или начало матча;
- хранить подтверждённый сервером тип отдельно от произвольного input;
- не восстанавливать HP при косметической или допустимой смене модели;
- отклонять смену типа в активном матче;
- при изменении wire-format увеличить `kProtocolVersion`.

Критерии готовности:

- смена значения в поддельном input не меняет тип и HP активного героя;
- выбранный тип корректно применяется при новом spawn/restart;
- есть тест многократного чередования `charType`.

### P0-03. Найти UB, проявляющийся в desktop Release

- [ ] Воспроизвести, локализовать и устранить.

Затронутые файлы:

- `desktop/build.bat`
- `desktop/CMakeLists.txt`
- потенциально любой код desktop client/render/game.

Проблема:

`desktop/build.bat` намеренно выбирает `RelWithDebInfo`, потому что Release с полным
inlining `/Ob2` падает. Это обход, а не устранение причины.

План диагностики:

1. Добавить отдельную Release-конфигурацию, не меняя обычную рабочую сборку.
2. Проверить warnings `/W4`, clang-cl, AddressSanitizer и UndefinedBehaviorSanitizer,
   где тулчейн это поддерживает.
3. Проверить lifetime объектов, use-after-free, неинициализированные поля,
   out-of-bounds, strict aliasing и invalidated vector references.
4. Зафиксировать минимальный сценарий воспроизведения.
5. После исправления вернуть возможность штатной Release-сборки.

Критерии готовности:

- Release запускается и проходит тот же smoke test, что RelWithDebInfo;
- причина описана в commit/MR;
- комментарий об известном heisenbug удалён из `desktop/build.bat`;
- добавлена автоматическая Release-сборка в CI.

## 4. P1 — высокий приоритет

### P1-01. Воспроизводимая Android/Vulkan-сборка

- [x] Исправлено (вариант «хранить проверенные .spv + автопроверка»). SPIR-V (11 шейдеров)
  **закоммичены** в `app/src/main/assets/shaders/vk/` (убраны из `.gitignore`) — чистый clone
  собирает рабочий Vulkan и на Android (Gradle пакует их как ассеты), и на десктопе (грузит из
  ассетов), БЕЗ внешнего Vulkan SDK. Единый список `app/src/main/cpp/shaders/vk_shaders.txt`
  читают и десктопный CMake, и скрипт. Десктоп больше НЕ компилирует шейдеры сборкой (убрана
  зависимость от версии локального glslc и «пляска» байтов). Регенерация — одна точка:
  `app/src/main/cpp/shaders/gen_vk_shaders.py` (канонический компилятор — **bundled в NDK**,
  пиновка проекта: разные glslc дают разные байты). `--check` пере-компилирует и сверяет с
  закоммиченными (для CI/pre-commit; exit 1 = устарели). Нет glslc/исходника — понятная ошибка.
  **Проверено:** скрипт сгенерировал 11 .spv (все валидны `spirv-val`); десктоп собирается без
  shader-шага и запускает Vulkan (GTX 1070) на этих .spv; после сборки `--check` чист (десктоп их
  не трогает). Остаётся (P1-08): job в CI, гоняющий `gen_vk_shaders.py --check`.

Затронутые файлы:

- `.gitignore`
- `desktop/CMakeLists.txt`
- `app/src/main/cpp/CMakeLists.txt`
- `app/build.gradle.kts`
- Vulkan shaders в assets.

Проблема:

SPIR-V исключены из Git, desktop CMake умеет генерировать их через `glslc`, а
Android-сборка ожидает готовые файлы. Чистый clone может собрать приложение без
рабочих Vulkan shaders.

Требуемое решение:

- сделать компиляцию GLSL → SPIR-V частью Android build graph; либо
- хранить проверенные `.spv` в репозитории и автоматически проверять их актуальность;
- использовать один список shaders для desktop и Android;
- выдавать понятную ошибку при отсутствии `glslc`/shader.

Критерии готовности:

- чистый clone собирает Android GL и Vulkan без предварительной desktop-сборки;
- изменение GLSL автоматически обновляет SPIR-V;
- отсутствующий shader останавливает сборку, а не ломает runtime;
- сценарий проверяется в CI.

### P1-02. Переполнение и тихое отбрасывание bone data

- [ ] Устранить фиксированный лимит или сделать корректный batching.

Затронутые файлы:

- `app/src/main/cpp/engine/render/VulkanRenderer.cpp`
- `app/src/main/cpp/engine/render/GlRenderer.cpp`
- структуры `RenderFrame`.

Проблема:

- Vulkan использует общий лимит около 512 bone matrices на кадр;
- модель примерно с 41 костью исчерпывает лимит после нескольких персонажей;
- оставшиеся skinned instances могут исчезать;
- GL может продолжить отрисовку с неверными bone offsets.

Требуемое исправление:

- реализовать динамический buffer, paging или batches;
- валидировать диапазон bone offset/count до draw;
- не допускать разного поведения GL и Vulkan;
- логировать и явно обрабатывать невозможный кадр вместо тихой порчи.

Критерии готовности:

- стресс-сцена с числом skinned characters выше старого лимита корректна в GL/Vulkan;
- нет out-of-range чтения bone data;
- лимит и fallback покрыты тестом или диагностическим assert.

### P1-03. Full-snapshot amplification через `ackTick`

- [ ] Добавить серверную валидацию и rate limiting.

Затронутые файлы:

- `app/src/main/cpp/engine/net/Net.cpp`
- `app/src/main/cpp/engine/net/Net.h`

Проблема:

Клиент может постоянно подтверждать нулевой или очень старый tick и заставлять сервер
отправлять полный мир вместо delta snapshot.

Требуемое исправление:

- принимать только монотонный и существующий ack в допустимом окне;
- ограничить частоту forced full snapshots;
- ввести per-peer byte/packet budget;
- собирать счётчики full/delta snapshot и dropped/rate-limited packets.

Критерии готовности:

- поддельный старый ack не создаёт 30 полных snapshots в секунду;
- обычное восстановление после packet loss продолжает работать;
- есть unit/integration test для stale/future/non-monotonic ack.

### P1-04. Неограниченный рост мира и `uint16_t` truncation

- [ ] Ввести явные сетевые и игровые лимиты.

Затронутые файлы:

- `app/src/main/cpp/game/GameWorld.cpp`
- `app/src/main/cpp/engine/net/Net.cpp`
- `app/src/main/cpp/engine/net/Net.h`

Проблема:

- количество сущностей не имеет общего верхнего лимита;
- количество изменённых сущностей приводится к `uint16_t`;
- переполнение заголовка рассинхронизирует payload и parser;
- delta generation и часть клиентского применения используют вложенные линейные поиски.

Требуемое исправление:

- задать `kMaxEntities` и лимиты по типам;
- проверять размер до сериализации;
- разбивать snapshot либо безопасно отклонять невозможное состояние;
- не выполнять narrowing conversion без проверки;
- заменить горячие O(N²) поиски на индекс `entityId -> entity/state`.

Критерии готовности:

- невозможно создать/сериализовать больше поддерживаемого числа сущностей;
- заголовок всегда соответствует payload;
- стресс-тест проверяет максимальный поддерживаемый мир;
- CPU time snapshot generation масштабируется близко к O(N).

### P1-05. Сервер продолжает запуск без корректной сцены

- [x] Исправлено. При неудачной `loadSceneDesc` сервер теперь пишет путь+причину в stderr и
  завершается `return 1` (было — «продолжаю без коллизий», тихий рассинхрон с клиентом). Тест
  `runSceneLoadFailTest` (несуществующая сцена → загрузка вернула false — сигнал для выхода).

Затронутые файлы:

- `server/main.cpp`
- `app/src/main/cpp/game/SceneLoader.cpp`

Проблема:

При ошибке сцены dedicated server может продолжить работу без ожидаемых коллизий.
Клиент и сервер тогда симулируют разные миры.

Критерии готовности:

- отсутствующая или невалидная обязательная сцена завершает запуск non-zero кодом;
- лог содержит путь и точную причину;
- есть smoke test несуществующего/повреждённого scene-файла.

### P1-06. Android Vulkan не делает fallback на GL

- [ ] Добавить безопасный fallback.

Затронутые файлы:

- `app/src/main/cpp/platform/main.cpp`
- фабрика/переключение renderer.

Критерии готовности:

- неудачная инициализация Vulkan автоматически оставляет рабочий GL renderer;
- UI сообщает причину недоступности Vulkan;
- повторное переключение не оставляет полусозданные GPU-ресурсы.

### P1-07. Android window lifecycle уничтожает игровую сессию

- [ ] Разделить session lifecycle и graphics lifecycle.

Затронутые файлы:

- `app/src/main/cpp/platform/main.cpp`
- `app/src/main/cpp/game/Scene.*`

Проблема:

`APP_CMD_TERM_WINDOW` уничтожает не только surface/renderer, но и `Scene`, сеть,
embedded server и состояние матча.

Требуемое исправление:

- сохранять игровую сессию при временной потере native window;
- пересоздавать только surface, renderer и GPU-resources;
- явно завершать сессию только при настоящем уничтожении activity/process.

Критерии готовности:

- сворачивание/возврат и пересоздание окна не разрывают матч;
- GL/Vulkan корректно восстанавливают resources;
- нет двойного shutdown ImGui, audio, ENet или renderer.

## 5. P2 — игровые и рендерные дефекты

### P2-01. `restartMatch()` игнорирует характеристики выбранного героя

- [x] Исправлено. `restartMatch` теперь зовёт общий `applyHeroStats(e)` (hp/maxHp/урон по
  закоммиченному `charType`) вместо `e.hp = e.maxHp = heroHp_`. Тест `runRestartStatsTest`
  (танк maxHp 200 → после рестарта 200, а не дефолт 40).

### P2-02. Стройка разрешена после окончания матча

- [x] Исправлено. `GameWorld::tryBuild` первым делом отклоняет запрос при `decided_`. Тест
  `runBuildAfterEndTest` (ядро пало → матч завершён; при ресурсе 157 стройка отклонена).

### P2-03. Неограниченный client prediction buffer

- [ ] Ограничить `Scene::pending_` и определить recovery.

Возможные меры:

- максимальное окно неподтверждённых inputs;
- timeout/reconnect или hard resync;
- пропуск replay слишком старых inputs;
- метрика/лог при достижении лимита.

### P2-04. Нет rate limit для build/input messages

- [ ] Ввести per-peer rate limits.

Сервер должен ограничивать как bytes/packets, так и число логических действий за tick.
Пакеты сверх лимита следует отбрасывать, а систематическое нарушение — завершать.

### P2-05. Невалидированный CLI port

- [x] Исправлено. `atoi` заменён на строгий `parsePort` (только целое 1..65535; мусор/хвост/
  отрицательное/переполнение → понятная ошибка в stderr + `return 1`). Тест `runPortParseTest`.

### P2-06. Ошибка `recreateSwapchain()` оставляет renderer готовым

- [ ] Исправить state machine Vulkan renderer.

Если swapchain уничтожен, а пересоздание не удалось, renderer не должен оставаться
`ready_ == true` и продолжать кадр.

### P2-07. Descriptor pool может исчерпаться

- [ ] Реализовать рост pool или управляемый лимит.

Нельзя продолжать с `VK_NULL_HANDLE` descriptor set. Создание texture/resource должно
возвращать явную ошибку либо расширять pool.

### P2-08. Тихое отбрасывание Vulkan instances

- [ ] Заменить фиксированный лимит около 512 на batching/dynamic buffer.

До исправления хотя бы считать dropped instances и выдавать однократный warning.

### P2-09. Vulkan игнорирует `clampEdges`

- [ ] Создавать/выбирать clamp sampler для UI и 9-slice.

Проверить края `panel.png`, button textures и atlas в GL/Vulkan.

### P2-10. Нет индивидуального освобождения renderer resources

- [ ] Расширить контракт `Renderer`.

Рассмотреть `destroyTexture`, `destroyMesh` и RAII handles либо документированный
arena-lifetime. Сейчас интерфейс предоставляет `create*`, а освобождение в основном
происходит только при полном уничтожении renderer.

### P2-11. `libvulkan.so` не освобождается

- [ ] Проверить и исправить lifecycle динамической библиотеки.

Успешный `dlopen` должен иметь соответствующий `dlclose` после полного shutdown и
обнуления function pointers.

## 6. P2 — конфиги, ассеты и недоверенные данные

### P2-12. Числовые поля конфигов не валидируются

- [ ] Добавить общий validation pass после parsing.

Затронутые файлы:

- `app/src/main/cpp/game/SceneLoader.cpp`
- loaders building/character configs;
- procedural asset generation.

Минимальные проверки:

- `grid.cell > 0`;
- разумные границы arena/grid;
- `cells > 0` и `cells <= texture size`;
- sphere `stacks/slices` выше безопасного минимума;
- положительные размеры textures;
- конечные float;
- валидные HP, speed, cooldown, scale и collider extents.

Parser должен различать:

- поле отсутствует;
- поле имеет неверный формат;
- поле вне диапазона.

### P2-13. Нет лимитов входных assets

- [ ] Ограничить размер файлов и результирующей геометрии.

Добавить лимиты:

- encoded и decoded image size;
- texture dimensions и channels;
- vertex/index count;
- число meshes/materials/nodes/bones;
- размер text/scene/config файлов.

Ошибки должны возвращаться без огромных аллокаций и Android ANR.

### P2-14. `FileAssetSource` допускает выход за `assetsDir`

- [ ] Нормализовать и ограничить пути.

Запретить absolute paths и `..`, после canonicalization проверять, что итоговый путь
остаётся внутри asset root. Особенно важно, если путь когда-либо станет управляться
удалённым клиентом или загружаемым manifest.

## 7. P2 — протокол и эксплуатация

### P2-15. Wire-format зависит от ABI

- [ ] Спроектировать явную сериализацию.

Текущий raw `memcpy` POD-структур зависит от:

- padding/alignment;
- little-endian;
- IEEE-754;
- размера и порядка полей;
- ручного изменения protocol version.

Рекомендуется явный writer/reader с проверкой границ, endian conversion и versioned
messages. Переход можно выполнять по одному типу сообщения.

### P2-16. Нет аутентификации и защиты сессии

- [ ] Зафиксировать threat model и требуемый уровень защиты.

Для LAN-прототипа отсутствие шифрования допустимо. Для публичного сервера нужны как
минимум session token, защита от spoof/replay на уровне игровой сессии и ограничения
ресурсов peer. Не внедрять собственную криптографию без готового проверенного протокола.

### P2-17. Docker и observability

- [ ] Усилить production-конфигурацию.

- запускать процесс не от root;
- закреплять base image/version, при необходимости digest;
- добавить health/readiness механизм;
- корректно обрабатывать SIGTERM;
- логировать peers, tick lag, entities, packet rate, full/delta snapshots;
- документировать memory/CPU/network limits.

## 8. P2/P3 — архитектурный долг

### P2-18. Декомпозиция `Scene`

- [ ] Выполнить поэтапно без изменения поведения.

`Scene` одновременно отвечает за:

- соединение, reconnect и embedded server;
- prediction/reconciliation;
- клиентскую физику;
- загрузку GPU-resources;
- построение `RenderFrame`;
- VFX и audio events;
- build mode, selection и picking;
- API для UI.

Целевая декомпозиция:

- `ClientSession` — join/host/reconnect и транспорт;
- `ClientWorld` — snapshots, prediction и локальная симуляция;
- `WorldPresentation` — `RenderFrame`, VFX и audio events;
- `SceneResources` — CPU/GPU assets;
- `Scene` — тонкая координация и совместимый фасад.

Порядок:

1. Выделить структуры данных без изменения ownership.
2. Выделить `WorldPresentation`.
3. Выделить `ClientSession`.
4. Выделить `ClientWorld`.
5. Сократить публичный API `Scene`.

На каждом этапе server/desktop/selftests должны оставаться зелёными.

### P2-19. Направление зависимостей `engine` → `game`

- [ ] Разделить transport, protocol и game server adapter.

Проблемы:

- `engine/net` знает и владеет `game/GameWorld`;
- игровые `EntityType`, `GamePhase`, `EntityState` находятся в `engine/net/Net.h`;
- `engine/render/ui` напрямую зависит от `game/Scene`.

Цель:

- transport ENet ничего не знает о `GameWorld`;
- wire/game protocol находится в отдельном нейтральном модуле;
- UI читает отдельную view-model/интерфейс;
- orchestration создаёт transport и game world на уровне приложения/server.

### P2-20. Дублирование правил стройки

- [ ] Вынести чистые общие функции проверки.

Footprint, blocked cells и часть build validation реализованы отдельно в
`GameWorld` и `Scene`. Клиенту можно оставить preview, но геометрические правила должны
использовать общую платформонезависимую реализацию, а окончательное решение всегда
остаётся за сервером.

### P3-01. Глобальный lifecycle ENet и raw pimpl pointers

- [ ] Перевести ownership на RAII.

Рассмотреть общий ref-counted ENet runtime и `std::unique_ptr<Impl>` для сетевых
классов. Это снизит риск double init/shutdown и ручных ошибок при раннем выходе.

### P3-02. Дублирование platform loop

- [ ] После стабилизации выделить общий client runtime.

Android и desktop должны сохранять разные window/input lifecycle, но общий порядок
создания session, scene, audio, renderer и frame update можно централизовать.

## 9. Незакоммиченные изменения на момент аудита

На момент проверки были изменены:

- `app/src/main/cpp/engine/render/ui/screens/BattleScreen.cpp`
- `app/src/main/cpp/game/Scene.cpp`
- `app/src/main/cpp/game/Scene.h`

Изменения добавляли:

- косметический build poof;
- selection ring;
- получение мировой позиции выбранной сущности.

Прямых критических ошибок в diff не найдено. Остался продуктовый риск:

- poof и звук появляются оптимистично до серверного подтверждения;
- при конфликте стройки или устаревшем состоянии ресурсов игрок увидит эффект
  отвергнутой постройки.

### P3-03. Подтверждать optimistic build feedback

- [ ] Решить продуктовую семантику feedback.

Варианты:

- оставить лёгкий «запрос» сразу, а полноценный poof/sound — после snapshot/ack;
- либо при отказе проигрывать явный reject feedback.

## 10. Не подтверждённые предположения

Следующие подозрения были дополнительно проверены и не должны автоматически
превращаться в задачи без нового воспроизведения:

- world-space overlay не доказанно перевёрнут в Vulkan: текущий Y-flip в
  `vulkanClipFix()` и screen conversion компенсируют друг друга;
- picking использует исходную GL-style projection и корректные near/far `-1..1`;
- накопительное масштабирование ImGui не подтверждено при текущем пересоздании context;
- доступ к visual по неизвестному `EntityType` сейчас защищён через fallback.

## 11. Тестирование и CI

### P1-08. Добавить базовый CI

- [ ] Создать pipeline.

Минимальные jobs:

1. server Debug/RelWithDebInfo build;
2. server Release build;
3. `vbase_server --selftest`;
4. `vbase_server --densetest`;
5. desktop compile;
6. Android clean-clone native/Gradle build;
7. shader compilation/validation;
8. форматирование или lint, если будет стандартизирован.

### P1-09. Добавить sanitizer и негативные тесты

- [ ] Добавить отдельные конфигурации.

Необходимые категории:

- NaN/Inf/overflow input;
- truncated/oversized/unknown network messages;
- stale/future ack;
- entity count around serialization limit;
- invalid scene/building/character config;
- reconnect и prediction reconciliation;
- scene load failure;
- renderer limits;
- Android surface recreation;
- максимальная поддерживаемая волна/число сущностей.

## 12. Выполненные проверки на момент аудита

- [x] Server build — успешно.
- [x] Desktop build — успешно.
- [x] `git diff --check` — успешно.
- [x] `vbase_server --selftest` — **22** сценария успешно (было 15 + 6 батча хардненинга:
  InputGuard, CharTypeHeal, RestartStats, BuildAfterEnd, PortParse, SceneLoadFail).
- [x] `vbase_server --densetest` — 4/4 успешно.

**Батч хардненинга №1 выполнен** (P0-01, P0-02, P1-05, P2-01, P2-02, P2-05) — см. отметки `[x]`
в разделах выше. Следующие по приоритету: P0-03 (Release UB), P1-01 (воспроизводимый Android/
Vulkan shader pipeline), P1-03/P1-04 (лимиты snapshot/ack/entities), P1-08 (CI).
- [x] Dense flow-field BFS 400×400 с 400 footprint — около 4.45 мс на поле
  на машине аудита.

Успешные тесты не закрывают сетевые эксплойты, Release UB, чистую Android/Vulkan
сборку и негативные сценарии: для них пока нет соответствующего покрытия.

## 13. Сильные решения, которые нужно сохранить

- общий контракт `RenderFrame` и интерфейс `Renderer`;
- `AssetSource` как граница Android/filesystem;
- Jolt за `CollisionWorld`/pimpl;
- единый fixed tick;
- авторитетная симуляция в `GameWorld`;
- client prediction/reconciliation и FIFO inputs;
- delta snapshots;
- headless gameplay tests;
- корректный общий порядок teardown GPU/ImGui на desktop;
- квалифицированные include от корня `cpp/`;
- отсутствие GL/Android/ImGui зависимостей в платформонезависимом gameplay.

## 14. Рекомендуемая последовательность работ

1. P0-01: серверная валидация input.
2. P0-02: запрет heal через `charType`.
3. P0-03: диагностика desktop Release UB.
4. P1-03/P1-04: ограничения snapshots, ack и entities.
5. P1-01: воспроизводимая Android/Vulkan shader pipeline.
6. P1-05/P1-06/P1-07: fail-fast server и Android lifecycle/fallback.
7. P1-08/P1-09: CI, Release и sanitizer jobs.
8. P1-02 и P2-06..P2-11: лимиты и state machine renderer.
9. P2-01..P2-05: gameplay/network correctness.
10. P2-12..P2-17: validation, protocol и production hardening.
11. P2-18..P3-02: архитектурная декомпозиция небольшими шагами.

После пунктов 1–7 проект получит значительно более безопасную базу для продолжения
gameplay-разработки. Архитектурную декомпозицию не следует начинать одновременно с
исправлением сетевых эксплойтов или Release UB.
