cmake_minimum_required(VERSION 3.15)
project(vbase_editor C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(APP "${CMAKE_CURRENT_SOURCE_DIR}/../app/src/main/cpp")
set(TP "${CMAKE_CURRENT_SOURCE_DIR}/../third_party")
set(ENET "${TP}/enet")
set(GLFW_DIR "${TP}/glfw")
set(IMGUI "${TP}/imgui")

# --- Jolt Physics (как в desktop): Scene тянет CollisionWorld -> Jolt. ---
set(JPH_USE_DX12 OFF CACHE BOOL "" FORCE)
set(JPH_USE_VK OFF CACHE BOOL "" FORCE)
set(JPH_USE_MTL OFF CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${TP}/jolt/Build" jolt_build)

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${GLFW_DIR}" glfw_build)

# Редактор — только GL-бэкенд, без сети-геймплея-UI-звука на рантайме, НО Scene тянет свои
# зависимости (Net/GameWorld/ClientSession/ClientWorld/Character/CollisionWorld), поэтому их
# компилируем. Vulkan/audio/GameUi/screens не нужны — не включаем.
add_executable(vbase_editor
        main.cpp
        ${APP}/game/Character.cpp
        ${APP}/engine/assets/Mesh.cpp
        ${APP}/engine/net/Net.cpp
        ${APP}/game/GameWorld.cpp
        ${APP}/game/FlowField.cpp
        ${APP}/game/NavDebug.cpp
        ${APP}/game/ClientSession.cpp
        ${APP}/game/ClientWorld.cpp
        ${APP}/engine/assets/Assets.cpp
        ${APP}/engine/assets/Model.cpp
        ${APP}/engine/assets/Font.cpp
        ${APP}/game/Scene.cpp
        ${APP}/game/SceneLoader.cpp
        ${APP}/game/CharacterRoster.cpp
        ${APP}/engine/physics/CollisionWorld.cpp
        ${APP}/engine/render/GlRenderer.cpp
        # UI-слой: GlRenderer::init бутстрапит ImGui-шрифт/скин через GameUi (жёсткая связка),
        # поэтому линкуем дерево UI, хотя редактор рисует свою панель, а не GameUi::build.
        ${APP}/engine/render/GameUi.cpp
        ${APP}/engine/render/UiSkin.cpp
        ${APP}/engine/render/ui/UiShell.cpp
        ${APP}/engine/render/ui/Dialogs.cpp
        ${APP}/engine/render/ui/screens/LoadingScreen.cpp
        ${APP}/engine/render/ui/screens/MainMenuScreen.cpp
        ${APP}/engine/render/ui/screens/BattleScreen.cpp
        ${APP}/engine/render/ui/screens/LobbyScreen.cpp
        ${APP}/engine/render/ui/panels/HomePanel.cpp
        ${APP}/engine/render/ui/panels/InventoryPanel.cpp
        ${APP}/engine/render/ui/panels/QuestsPanel.cpp
        ${APP}/engine/render/ui/panels/ShopPanel.cpp
        ${APP}/engine/render/ui/panels/EventsPanel.cpp
        ${APP}/engine/render/ui/windows/DebugPanel.cpp
        ${APP}/engine/render/ui/windows/BuildingInfo.cpp
        ${IMGUI}/imgui.cpp
        ${IMGUI}/imgui_draw.cpp
        ${IMGUI}/imgui_tables.cpp
        ${IMGUI}/imgui_widgets.cpp
        ${IMGUI}/backends/imgui_impl_opengl3.cpp
        ${IMGUI}/backends/imgui_impl_glfw.cpp
        ${TP}/imguizmo/ImGuizmo.cpp
        ${ENET}/callbacks.c
        ${ENET}/compress.c
        ${ENET}/host.c
        ${ENET}/list.c
        ${ENET}/packet.c
        ${ENET}/peer.c
        ${ENET}/protocol.c
        ${ENET}/win32.c)

target_include_directories(vbase_editor PRIVATE
        "${APP}" "${ENET}/include" "${TP}/cgltf" "${TP}/stb" "${IMGUI}" "${IMGUI}/backends"
        "${TP}/vulkan/include" "${TP}/imguizmo")

if(MSVC)
    target_compile_options(vbase_editor PRIVATE /utf-8)
endif()

target_link_libraries(vbase_editor glfw opengl32 ws2_32 winmm Jolt)
