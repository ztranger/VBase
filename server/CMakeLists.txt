# 3.20 — требование Jolt (его CMakeLists FATAL_ERROR ниже 3.20).
cmake_minimum_required(VERSION 3.20)
project(vbase_server C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Общий код клиента (симуляция + сеть) и ENet.
set(APP "${CMAKE_CURRENT_SOURCE_DIR}/../app/src/main/cpp")
set(TP "${CMAKE_CURRENT_SOURCE_DIR}/../third_party")
set(ENET "${TP}/enet")

# --- Jolt Physics (вендорено в third_party/jolt) ---
# Подключаем через РОДНОЙ CMake-таргет Jolt: он PUBLIC-ом раздаёт JPH-дефайны и
# include-путь, поэтому дефайны либы и потребителя совпадают автоматически
# (рассинхрон дефайнов Jolt = порча памяти). GPU-compute (DX12/VK/MTL) кинематике
# не нужен и тянет dxc/glslc — выключаем; CPU-compute (чистый C++) оставляем.
set(JPH_USE_DX12 OFF CACHE BOOL "" FORCE)
set(JPH_USE_VK OFF CACHE BOOL "" FORCE)
set(JPH_USE_MTL OFF CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)          # не ломать сборку /WX на новом MSVC
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE) # без LTO — мешает миксу со static-либой
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)  # /MD, как у наших целей
set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${TP}/jolt/Build" jolt_build)

# ENet: на Windows нужен win32.c (+ ws2_32/winmm), на Unix — unix.c.
if(WIN32)
    set(ENET_PLATFORM ${ENET}/win32.c)
else()
    set(ENET_PLATFORM ${ENET}/unix.c)
endif()

add_executable(vbase_server
        main.cpp
        ${APP}/engine/net/Net.cpp
        ${APP}/game/GameWorld.cpp
        ${APP}/game/FlowField.cpp
        ${APP}/game/NavDebug.cpp
        ${APP}/game/Character.cpp
        ${APP}/engine/physics/CollisionWorld.cpp
        ${APP}/game/SceneLoader.cpp
        ${APP}/game/CharacterRoster.cpp
        ${ENET}/callbacks.c
        ${ENET}/compress.c
        ${ENET}/host.c
        ${ENET}/list.c
        ${ENET}/packet.c
        ${ENET}/peer.c
        ${ENET}/protocol.c
        ${ENET_PLATFORM})

target_include_directories(vbase_server PRIVATE "${APP}" "${ENET}/include")

# MSVC по умолчанию читает исходники в системной кодировке — заставляем UTF-8,
# чтобы русские строки в бинарь попадали как UTF-8 (консоль тоже в UTF-8).
if(MSVC)
    target_compile_options(vbase_server PRIVATE /utf-8)
endif()

# P1-09: опциональная сборка с AddressSanitizer (MSVC /fsanitize=address). OFF по умолчанию —
# обычная сборка не затрагивается. Включается отдельным build-asan.bat в СВОЁМ build-каталоге,
# чтобы не смешивать инструментированные объектники с обычной сборкой. UBSan на cl.exe нет —
# для него нужен clang-cl (см. PROJECT_REVIEW P1-09).
option(VBASE_ASAN "Build vbase_server with AddressSanitizer (MSVC)" OFF)
if(VBASE_ASAN AND MSVC)
    target_compile_options(vbase_server PRIVATE /fsanitize=address /Zi)
    # Jolt.lib собран БЕЗ ASan -> отключаем ASan-аннотации STL-контейнеров (string/vector/optional),
    # иначе LNK2038 (mismatch annotate_*) при линковке с неинструментированной либой.
    target_compile_definitions(vbase_server PRIVATE _DISABLE_STL_ANNOTATION=1)
    target_link_options(vbase_server PRIVATE /INCREMENTAL:NO /DEBUG)  # ASan: без инкрем. линковки, +символы
    message(STATUS "vbase_server: AddressSanitizer ВКЛЮЧЁН")
endif()

if(NOT WIN32)
    set_source_files_properties(${ENET_PLATFORM} PROPERTIES COMPILE_DEFINITIONS
            "HAS_SOCKLEN_T=1;HAS_FCNTL=1;HAS_POLL=1;HAS_GETADDRINFO=1;HAS_GETNAMEINFO=1;HAS_GETHOSTBYNAME_R=1;HAS_GETHOSTBYADDR_R=1;HAS_INET_PTON=1;HAS_INET_NTOP=1;HAS_MSGHDR_FLAGS=1")
endif()

if(WIN32)
    target_link_libraries(vbase_server ws2_32 winmm)
else()
    # std::thread / Jolt на Linux. Сам Jolt тоже PUBLIC -pthread, это дубль на случай
    # линкера, который не протаскивает флаг со статической либы.
    find_package(Threads REQUIRED)
    target_link_libraries(vbase_server Threads::Threads)
endif()

# Jolt: линковка таргета тянет и его PUBLIC-дефайны/include (см. выше).
target_link_libraries(vbase_server Jolt)
