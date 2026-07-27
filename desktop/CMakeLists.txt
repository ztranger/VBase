cmake_minimum_required(VERSION 3.15)
project(vbase_desktop C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(APP "${CMAKE_CURRENT_SOURCE_DIR}/../app/src/main/cpp")
set(ENET "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/enet")
set(GLFW_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/glfw")

# GLFW из исходников (только сама либа, без примеров/тестов/доки).
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${GLFW_DIR}" glfw_build)

add_executable(vbase_desktop
        main.cpp
        DesktopRenderer.cpp
        ${APP}/Character.cpp
        ${APP}/Mesh.cpp
        ${APP}/Net.cpp
        ${ENET}/callbacks.c
        ${ENET}/compress.c
        ${ENET}/host.c
        ${ENET}/list.c
        ${ENET}/packet.c
        ${ENET}/peer.c
        ${ENET}/protocol.c
        ${ENET}/win32.c)

target_include_directories(vbase_desktop PRIVATE "${APP}" "${ENET}/include")

if(MSVC)
    target_compile_options(vbase_desktop PRIVATE /utf-8)
endif()

target_link_libraries(vbase_desktop glfw opengl32 ws2_32 winmm)
