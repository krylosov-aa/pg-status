FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        clang \
        cmake \
        libcjson-dev \
        libclang-rt-dev \
        libevent-dev \
        libpq-dev \
        ninja-build \
        pkg-config \
        valgrind && \
    rm -rf /var/lib/apt/lists/*

ARG PG_STATUS_E2E_BUILD_TYPE=Release
ARG PG_STATUS_E2E_SANITIZER=none

ENV CC=clang
WORKDIR /app

COPY CMakeLists.txt ./
COPY src/ src/

RUN cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE="${PG_STATUS_E2E_BUILD_TYPE}" \
        -DBUILD_TESTING=OFF \
        -DPG_STATUS_ENABLE_CLANG_TIDY=OFF \
        -DPG_STATUS_SANITIZER="${PG_STATUS_E2E_SANITIZER}" && \
    cmake --build build --target pg-status --parallel && \
    cp build/src/pg-status /app/pg-status

COPY test/e2e/entrypoint.sh /usr/local/bin/pg-status-e2e-entrypoint
RUN chmod 0755 /usr/local/bin/pg-status-e2e-entrypoint

ENTRYPOINT ["/usr/local/bin/pg-status-e2e-entrypoint"]
