cmake_minimum_required(VERSION 3.15)
project(vbase_server C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Общий код клиента (симуляция + сеть) и ENet.
set(APP "${CMAKE_CURRENT_SOURCE_DIR}/../app/src/main/cpp")
set(ENET "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/enet")

# ENet: на Windows нужен win32.c (+ ws2_32/winmm), на Unix — unix.c.
if(WIN32)
    set(ENET_PLATFORM ${ENET}/win32.c)
else()
    set(ENET_PLATFORM ${ENET}/unix.c)
endif()

add_executable(vbase_server
        main.cpp
        ${APP}/Net.cpp
        ${APP}/Character.cpp
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

if(NOT WIN32)
    set_source_files_properties(${ENET_PLATFORM} PROPERTIES COMPILE_DEFINITIONS
            "HAS_SOCKLEN_T=1;HAS_FCNTL=1;HAS_POLL=1;HAS_GETADDRINFO=1;HAS_GETNAMEINFO=1;HAS_GETHOSTBYNAME_R=1;HAS_GETHOSTBYADDR_R=1;HAS_INET_PTON=1;HAS_INET_NTOP=1;HAS_MSGHDR_FLAGS=1")
endif()

if(WIN32)
    target_link_libraries(vbase_server ws2_32 winmm)
endif()
