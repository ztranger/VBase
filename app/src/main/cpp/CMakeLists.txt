cmake_minimum_required(VERSION 3.22.1)
project(vbase CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# GameActivity приходит через Prefab из AAR androidx.games:games-activity
find_package(game-activity REQUIRED CONFIG)

# Сборка: игровой слой (Scene/Mesh) + GL-бэкенд + probe доступности Vulkan.
# VulkanRenderer.cpp временно исключён — портируется под контракт RenderFrame.
# (Шейдеры Vulkan через glslc вернутся вместе с ним.)
# Dear ImGui (вендрится в third_party/imgui). GL ES 3 бэкенд + ядро.
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party/imgui")

set(CGLTF_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party/cgltf")

add_library(vbase SHARED
        main.cpp
        Mesh.cpp
        Assets.cpp
        Font.cpp
        Model.cpp
        Scene.cpp
        GlRenderer.cpp
        VulkanProbe.cpp
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/imgui_demo.cpp
        ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)

target_include_directories(vbase PRIVATE "${IMGUI_DIR}" "${IMGUI_DIR}/backends" "${CGLTF_DIR}")
# Заставляет imgui_impl_opengl3 использовать GLES3, а не десктопный GL.
target_compile_definitions(vbase PRIVATE IMGUI_IMPL_OPENGL_ES3)

# GameActivity подгружается из Java через JNI-символ в статической библиотеке —
# заставляем линкер не выбрасывать его
set(CMAKE_SHARED_LINKER_FLAGS
        "${CMAKE_SHARED_LINKER_FLAGS} -u Java_com_google_androidgamesdk_GameActivity_initializeNativeCode")

target_link_libraries(vbase
        game-activity::game-activity_static
        android
        jnigraphics   # AImageDecoder_* для загрузки текстур
        EGL
        GLESv3
        vulkan
        log)
