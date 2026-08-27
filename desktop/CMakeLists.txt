cmake_minimum_required(VERSION 3.15)
project(vbase_desktop C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(APP "${CMAKE_CURRENT_SOURCE_DIR}/../app/src/main/cpp")
set(TP "${CMAKE_CURRENT_SOURCE_DIR}/../third_party")
set(ENET "${TP}/enet")
set(GLFW_DIR "${TP}/glfw")
set(IMGUI "${TP}/imgui")

# --- Jolt Physics (вендорено в third_party/jolt) ---
# Через родной CMake-таргет Jolt: он PUBLIC-ом раздаёт JPH-дефайны и include-путь,
# поэтому дефайны либы и потребителя совпадают автоматически. GPU-compute (DX12/VK/MTL)
# кинематике не нужен и тянет dxc/glslc — выключаем; CPU-compute (чистый C++) оставляем.
set(JPH_USE_DX12 OFF CACHE BOOL "" FORCE)
set(JPH_USE_VK OFF CACHE BOOL "" FORCE)
set(JPH_USE_MTL OFF CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)          # не ломать /WX на новом MSVC
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE) # без LTO — мешает миксу со static-либой
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)  # /MD, как у наших целей
set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${TP}/jolt/Build" jolt_build)

# GLFW из исходников (только сама либа, без примеров/тестов/доки).
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${GLFW_DIR}" glfw_build)

add_executable(vbase_desktop
        main.cpp
        ${APP}/game/Character.cpp
        ${APP}/engine/assets/Mesh.cpp
        ${APP}/engine/net/Net.cpp
        ${APP}/game/GameWorld.cpp
        ${APP}/game/FlowField.cpp
        ${APP}/engine/assets/Assets.cpp
        ${APP}/engine/assets/Model.cpp
        ${APP}/game/Scene.cpp
        ${APP}/game/SceneLoader.cpp
        ${APP}/game/CharacterRoster.cpp
        ${APP}/engine/physics/CollisionWorld.cpp
        ${APP}/engine/render/GlRenderer.cpp
        ${APP}/engine/render/VkApi.cpp
        ${APP}/engine/render/VulkanRenderer.cpp
        ${APP}/engine/render/GameUi.cpp
        ${APP}/engine/render/UiSkin.cpp
        ${APP}/engine/render/ui/UiShell.cpp
        ${APP}/engine/render/ui/Dialogs.cpp
        ${APP}/engine/render/ui/screens/LoadingScreen.cpp
        ${APP}/engine/render/ui/screens/MainMenuScreen.cpp
        ${APP}/engine/render/ui/screens/BattleScreen.cpp
        ${APP}/engine/render/ui/screens/CharacterSelectScreen.cpp
        ${APP}/engine/render/ui/panels/HomePanel.cpp
        ${APP}/engine/render/ui/windows/DebugPanel.cpp
        ${APP}/engine/render/ui/windows/BuildingInfo.cpp
        ${APP}/engine/assets/Font.cpp
        ${IMGUI}/imgui.cpp
        ${IMGUI}/imgui_draw.cpp
        ${IMGUI}/imgui_tables.cpp
        ${IMGUI}/imgui_widgets.cpp
        ${IMGUI}/backends/imgui_impl_opengl3.cpp
        ${IMGUI}/backends/imgui_impl_glfw.cpp
        ${IMGUI}/backends/imgui_impl_vulkan.cpp
        ${ENET}/callbacks.c
        ${ENET}/compress.c
        ${ENET}/host.c
        ${ENET}/list.c
        ${ENET}/packet.c
        ${ENET}/peer.c
        ${ENET}/protocol.c
        ${ENET}/win32.c)

# ImGui — БЕЗ IMGUI_IMPL_OPENGL_ES3: на десктопе бэкенд использует полный GL 3.3.
# vulkan/include — вендоренные заголовки Khronos (линковки не требуют: функции
# грузятся динамически через glfwGetInstanceProcAddress, vulkan-1.lib не нужен).
target_include_directories(vbase_desktop PRIVATE
        "${APP}" "${ENET}/include" "${TP}/cgltf" "${TP}/stb" "${IMGUI}" "${IMGUI}/backends"
        "${TP}/vulkan/include")

# imgui_impl_vulkan должен грузить Vulkan-функции через ImGui_ImplVulkan_LoadFunctions
# (у нас VK_NO_PROTOTYPES, линковать vulkan-1 не с чем).
target_compile_definitions(vbase_desktop PRIVATE IMGUI_IMPL_VULKAN_NO_PROTOTYPES)

if(MSVC)
    target_compile_options(vbase_desktop PRIVATE /utf-8)
endif()

target_link_libraries(vbase_desktop glfw opengl32 ws2_32 winmm Jolt)

# --- Vulkan-шейдеры: GLSL -> SPIR-V через glslc, в assets/shaders/vk (грузятся
# рендером через AssetSource). glslc из Vulkan SDK (переопределяется -DGLSLC=...).
set(GLSLC "C:/VulkanSDK/1.4.350.0/Bin/glslc.exe" CACHE FILEPATH "Путь к glslc")
set(VK_SHADER_SRC "${APP}/shaders")
set(VK_SHADER_OUT "${APP}/../assets/shaders/vk")
file(MAKE_DIRECTORY "${VK_SHADER_OUT}")
set(VK_SHADERS lit.vert lit.frag unlit.frag phong.vert phong.frag skin.vert skin.frag hud.vert hud.frag)
set(VK_SPV "")
foreach(s ${VK_SHADERS})
    add_custom_command(
            OUTPUT "${VK_SHADER_OUT}/${s}.spv"
            COMMAND "${GLSLC}" "${VK_SHADER_SRC}/${s}" -o "${VK_SHADER_OUT}/${s}.spv"
            DEPENDS "${VK_SHADER_SRC}/${s}"
            COMMENT "glslc ${s} -> ${s}.spv"
            VERBATIM)
    list(APPEND VK_SPV "${VK_SHADER_OUT}/${s}.spv")
endforeach()
add_custom_target(vk_shaders DEPENDS ${VK_SPV})
add_dependencies(vbase_desktop vk_shaders)
