cmake_minimum_required(VERSION 3.22.1)
project(vbase C CXX)  # C нужен для исходников ENet (.c)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# GameActivity приходит через Prefab из AAR androidx.games:games-activity
find_package(game-activity REQUIRED CONFIG)

# Сборка: игровой слой + оба рендер-бэкенда (GL ES3 и Vulkan). Vulkan-функции
# грузятся динамически (VkApi, dlopen libvulkan) — libvulkan НЕ линкуется, иначе
# имена-указатели VkApi конфликтуют с экспортами. Заголовки Vulkan — вендоренные.
# Dear ImGui (вендрится в third_party/imgui): GL ES3 + Vulkan бэкенды + ядро.
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party/imgui")
set(VULKAN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party/vulkan")
set(JOLT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party/jolt")

# --- Jolt Physics (вендорено в third_party/jolt) ---
# Через родной CMake-таргет Jolt: PUBLIC-дефайны и include-путь совпадают у либы и
# потребителя автоматически. GPU-compute (DX12/VK/MTL) кинематике не нужен: на Android
# JPH_USE_VK по умолчанию ON и потянул бы find_package(Vulkan)+glslc — выключаем всё,
# CPU-compute (чистый C++) оставляем. Без LTO (на ARM ломает кодоген) и без /WX.
set(JPH_USE_DX12 OFF CACHE BOOL "" FORCE)
set(JPH_USE_VK OFF CACHE BOOL "" FORCE)
set(JPH_USE_MTL OFF CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${JOLT_DIR}/Build" jolt_build)

# ENet (UDP-транспорт). Компилируем исходники напрямую, минуя его CMake.
# win32.c пропущен — на Android нужен unix.c (гарды внутри файлов).
set(ENET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party/enet")
set(ENET_SOURCES
        ${ENET_DIR}/callbacks.c
        ${ENET_DIR}/compress.c
        ${ENET_DIR}/host.c
        ${ENET_DIR}/list.c
        ${ENET_DIR}/packet.c
        ${ENET_DIR}/peer.c
        ${ENET_DIR}/protocol.c
        ${ENET_DIR}/unix.c)

# ENet сам определяет эти возможности через свой CMake; мы его не используем,
# поэтому задаём вручную (иначе unix.c переопределит socklen_t и т.п.).
set_source_files_properties(${ENET_SOURCES} PROPERTIES COMPILE_DEFINITIONS
        "HAS_SOCKLEN_T=1;HAS_FCNTL=1;HAS_POLL=1;HAS_GETADDRINFO=1;HAS_GETNAMEINFO=1;HAS_GETHOSTBYNAME_R=1;HAS_GETHOSTBYADDR_R=1;HAS_INET_PTON=1;HAS_INET_NTOP=1;HAS_MSGHDR_FLAGS=1")

set(CGLTF_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party/cgltf")
set(STB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party/stb")

add_library(vbase SHARED
        platform/main.cpp
        engine/assets/Mesh.cpp
        engine/assets/Assets.cpp
        engine/assets/Font.cpp
        engine/assets/Model.cpp
        game/Character.cpp
        engine/net/Net.cpp
        game/GameWorld.cpp
        game/Scene.cpp
        game/SceneLoader.cpp
        game/CharacterRoster.cpp
        engine/physics/CollisionWorld.cpp
        engine/render/GlRenderer.cpp
        engine/render/GameUi.cpp
        engine/render/UiSkin.cpp
        engine/render/ui/UiShell.cpp
        engine/render/ui/Dialogs.cpp
        engine/render/ui/screens/LoadingScreen.cpp
        engine/render/ui/screens/MainMenuScreen.cpp
        engine/render/ui/screens/BattleScreen.cpp
        engine/render/ui/screens/CharacterSelectScreen.cpp
        engine/render/ui/panels/HomePanel.cpp
        engine/render/ui/windows/DebugPanel.cpp
        engine/render/ui/windows/BuildingInfo.cpp
        engine/render/VulkanProbe.cpp
        engine/render/VkApi.cpp
        engine/render/VulkanRenderer.cpp
        ${ENET_SOURCES}
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
        ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp)

target_include_directories(vbase PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
        "${IMGUI_DIR}" "${IMGUI_DIR}/backends"
        "${VULKAN_DIR}/include" "${CGLTF_DIR}" "${STB_DIR}" "${ENET_DIR}/include")
# imgui_impl_opengl3 -> GLES3; imgui_impl_vulkan -> загрузка функций (VK_NO_PROTOTYPES).
target_compile_definitions(vbase PRIVATE IMGUI_IMPL_OPENGL_ES3 IMGUI_IMPL_VULKAN_NO_PROTOTYPES)

# GameActivity подгружается из Java через JNI-символ в статической библиотеке —
# заставляем линкер не выбрасывать его
set(CMAKE_SHARED_LINKER_FLAGS
        "${CMAKE_SHARED_LINKER_FLAGS} -u Java_com_google_androidgamesdk_GameActivity_initializeNativeCode")

# libvulkan НЕ линкуем — грузим через dlopen (см. VkApi). Нужен только dl (dlopen).
# Jolt: линковка таргета тянет и его PUBLIC-дефайны/include (см. выше).
target_link_libraries(vbase
        game-activity::game-activity_static
        android
        EGL
        GLESv3
        dl
        log
        Jolt)
