cmake_minimum_required(VERSION 3.15)
project(vbase_desktop C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(APP "${CMAKE_CURRENT_SOURCE_DIR}/../app/src/main/cpp")
set(TP "${CMAKE_CURRENT_SOURCE_DIR}/../third_party")
set(ENET "${TP}/enet")
set(GLFW_DIR "${TP}/glfw")
set(IMGUI "${TP}/imgui")

# GLFW из исходников (только сама либа, без примеров/тестов/доки).
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${GLFW_DIR}" glfw_build)

add_executable(vbase_desktop
        main.cpp
        ${APP}/Character.cpp
        ${APP}/Mesh.cpp
        ${APP}/Net.cpp
        ${APP}/Assets.cpp
        ${APP}/Model.cpp
        ${APP}/Scene.cpp
        ${APP}/GlRenderer.cpp
        ${APP}/GameUi.cpp
        ${APP}/Font.cpp
        ${IMGUI}/imgui.cpp
        ${IMGUI}/imgui_draw.cpp
        ${IMGUI}/imgui_tables.cpp
        ${IMGUI}/imgui_widgets.cpp
        ${IMGUI}/backends/imgui_impl_opengl3.cpp
        ${IMGUI}/backends/imgui_impl_glfw.cpp
        ${ENET}/callbacks.c
        ${ENET}/compress.c
        ${ENET}/host.c
        ${ENET}/list.c
        ${ENET}/packet.c
        ${ENET}/peer.c
        ${ENET}/protocol.c
        ${ENET}/win32.c)

# ImGui — БЕЗ IMGUI_IMPL_OPENGL_ES3: на десктопе бэкенд использует полный GL 3.3.
target_include_directories(vbase_desktop PRIVATE
        "${APP}" "${ENET}/include" "${TP}/cgltf" "${TP}/stb" "${IMGUI}" "${IMGUI}/backends")

if(MSVC)
    target_compile_options(vbase_desktop PRIVATE /utf-8)
endif()

target_link_libraries(vbase_desktop glfw opengl32 ws2_32 winmm)
