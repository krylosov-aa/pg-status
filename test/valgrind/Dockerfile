# -------- Build --------
FROM debian:stable-slim AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        pkg-config \
        libpq-dev \
        libmicrohttpd-dev \
        libcjson-dev && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY CMakeLists.txt .
COPY src/ src/

RUN cmake -B build -DCMAKE_BUILD_TYPE=Debug . && \
    cmake --build build --config Debug

# -------- Run --------
FROM debian:stable-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        libpq5 \
        libmicrohttpd12 \
        libcjson1 \
        valgrind && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY --from=builder /app/build/src/main /app/main

ENTRYPOINT ["valgrind", "--leak-check=full", "--show-leak-kinds=all", "--track-origins=yes", "--num-callers=50", "/app/main"]
