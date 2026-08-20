FROM ubuntu:latest

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        clang \
        clang-tidy \
        clang-tools \
        cmake \
        libcjson-dev \
        libclang-rt-dev \
        libevent-dev \
        libpq-dev \
        make \
        ninja-build \
        pkg-config \
        valgrind && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY Makefile CMakeLists.txt .clang-tidy ./
COPY src/ src/
COPY test/CMakeLists.txt test/
COPY test/tests/ test/tests/
