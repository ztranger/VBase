cmake_minimum_required(VERSION 3.22.1)
project(vbase C CXX)  # C нужен для исходников ENet (.c)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# GameActivity приходит через Prefab из AAR androidx.games:games-activity
find_package(game-activity REQUIRED CONFIG)

# Сборка: игровой слой (Scene/Mesh) + GL-бэкенд + probe доступности Vulkan.
# VulkanRenderer.cpp временно исключён — портируется под контракт RenderFrame.
# (Шейдеры Vulkan через glslc вернутся вместе с ним.)
# Dear ImGui (вендрится в third_party/imgui). GL ES 3 бэкенд + ядро.
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party/imgui")

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
        main.cpp
        Mesh.cpp
        Assets.cpp
        Font.cpp
        Model.cpp
        Character.cpp
        Net.cpp
        Scene.cpp
        GlRenderer.cpp
        GameUi.cpp
        VulkanProbe.cpp
        ${ENET_SOURCES}
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)

target_include_directories(vbase PRIVATE "${IMGUI_DIR}" "${IMGUI_DIR}/backends" "${CGLTF_DIR}" "${STB_DIR}" "${ENET_DIR}/include")
# Заставляет imgui_impl_opengl3 использовать GLES3, а не десктопный GL.
target_compile_definitions(vbase PRIVATE IMGUI_IMPL_OPENGL_ES3)

# GameActivity подгружается из Java через JNI-символ в статической библиотеке —
# заставляем линкер не выбрасывать его
set(CMAKE_SHARED_LINKER_FLAGS
        "${CMAKE_SHARED_LINKER_FLAGS} -u Java_com_google_androidgamesdk_GameActivity_initializeNativeCode")

target_link_libraries(vbase
        game-activity::game-activity_static
        android
        EGL
        GLESv3
        vulkan
        log)
