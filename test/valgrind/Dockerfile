# -------- Build --------
FROM debian:stable-slim AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        pkg-config \
        libpq-dev \
        libevent-dev \
        libcjson-dev && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY CMakeLists.txt .
COPY src/ src/
COPY test/CMakeLists.txt test/
COPY test/tests/ test/tests/

RUN cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release . && \
    cmake --build build && \
    ctest --test-dir build --output-on-failure

# -------- Run --------
FROM debian:stable-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        libpq5 \
        libevent-2.1-7t64 \
        libcjson1 \
        valgrind && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY --from=builder /app/build/src/pg-status /app/pg-status

ENTRYPOINT ["valgrind", "--leak-check=full", "--show-leak-kinds=all", "--track-origins=yes", "--num-callers=50", "/app/pg-status"]
