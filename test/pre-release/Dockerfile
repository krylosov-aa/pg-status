FROM ubuntu:24.04

ARG CLANG_FORMAT_VERSION=22.1.8
ARG CLANG_FORMAT_AMD64_SHA256=b00cff6bfd1f1686f073a4fdf1cb937dbd58bf7510c659477805c03afdea0816
ARG CLANG_FORMAT_AMD64_URL=https://files.pythonhosted.org/packages/e5/88/b82c066fa807da4ca2518fecf79071361f6324b77375e5e92c059c0697fd/clang_format-22.1.8-py2.py3-none-manylinux_2_27_x86_64.manylinux_2_28_x86_64.whl
ARG CLANG_FORMAT_ARM64_SHA256=48c3b8dcfe9d4e964ced0e744e0f1f8ddc711bce92e50f6cab21e10f54857d08
ARG CLANG_FORMAT_ARM64_URL=https://files.pythonhosted.org/packages/50/25/a9734da014eecc1f54c051ad643a28f2f6643dcc812ac59320e80e2b1a3b/clang_format-22.1.8-py2.py3-none-manylinux_2_26_aarch64.manylinux_2_28_aarch64.whl
ARG TARGETARCH

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        clang \
        clang-tidy \
        clang-tools \
        cmake \
        curl \
        libcjson-dev \
        libclang-rt-dev \
        libevent-dev \
        libpq-dev \
        make \
        ninja-build \
        pkg-config \
        python3 \
        valgrind && \
    rm -rf /var/lib/apt/lists/*

RUN architecture="${TARGETARCH:-$(dpkg --print-architecture)}" && \
    case "${architecture}" in \
        amd64) url="${CLANG_FORMAT_AMD64_URL}"; sha256="${CLANG_FORMAT_AMD64_SHA256}" ;; \
        arm64) url="${CLANG_FORMAT_ARM64_URL}"; sha256="${CLANG_FORMAT_ARM64_SHA256}" ;; \
        *) echo "unsupported clang-format architecture: ${architecture}" >&2; exit 1 ;; \
    esac && \
    wheel="/tmp/clang-format-${CLANG_FORMAT_VERSION}.whl" && \
    curl -fSL "${url}" -o "${wheel}" && \
    echo "${sha256}  ${wheel}" | sha256sum -c - && \
    python3 -m zipfile -e "${wheel}" /tmp/clang-format-wheel && \
    install -m 0755 \
        /tmp/clang-format-wheel/clang_format/data/bin/clang-format \
        /usr/local/bin/clang-format && \
    rm -rf "${wheel}" /tmp/clang-format-wheel

WORKDIR /app

COPY Makefile CMakeLists.txt CMakePresets.json .clang-format .clang-tidy ./
COPY src/ src/
COPY test/CMakeLists.txt test/
COPY test/tests/ test/tests/
