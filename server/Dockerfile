# Выделенный сервер VBase под Linux. Контекст сборки — корень репозитория:
#   docker build -t vbase-server -f server/Dockerfile .
#
# Не Windows-контейнер: тот же C++ собирается gcc/CMake (ENet unix.c, POSIX).
# Jolt в образе — минуты на первую сборку.

FROM ubuntu:24.04 AS builder
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        g++ cmake ninja-build \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY third_party/jolt /src/third_party/jolt
COPY third_party/enet /src/third_party/enet
COPY server/CMakeLists.txt server/main.cpp /src/server/
COPY app/src/main/cpp /src/app/src/main/cpp

RUN cmake -S /src/server -B /src/server/build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DGENERATE_DEBUG_SYMBOLS=OFF \
    && cmake --build /src/server/build \
    && /src/server/build/vbase_server --selftest \
    && strip /src/server/build/vbase_server

FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive

WORKDIR /app
COPY --from=builder /src/server/build/vbase_server /app/vbase_server
COPY app/src/main/assets/scenes /app/assets/scenes
COPY app/src/main/assets/config /app/assets/config

EXPOSE 7777/udp
ENTRYPOINT ["/app/vbase_server"]
CMD ["7777", "/app/assets", "scenes/default.scene"]
