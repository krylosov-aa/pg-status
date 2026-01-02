# -------- Build --------
FROM alpine AS builder

RUN apk add --no-cache \
        bash \
        clang \
        clang-extra-tools \
        build-base \
        cmake \
        pkgconfig \
        libtool \
        autoconf \
        automake \
        tar \
        curl \
        libpq-dev \
        cjson-dev

WORKDIR /tmp

RUN curl -LO https://ftp.gnu.org/gnu/libmicrohttpd/libmicrohttpd-1.0.2.tar.gz && \
    tar xf libmicrohttpd-1.0.2.tar.gz && \
    cd libmicrohttpd-1.0.2 && \
    ./configure --disable-https && \
    make -j$(nproc) && \
    make install

WORKDIR /app

COPY CMakeLists.txt .
COPY src/ src/

ENV CC=clang

RUN cmake -B build -DCMAKE_BUILD_TYPE=Release . && \
    cmake --build build --config Release

# -------- Run --------
FROM alpine:3.20

RUN apk add --no-cache \
        libpq \
        cjson

WORKDIR /app

COPY --from=builder /usr/local/lib/libmicrohttpd.so* /usr/local/lib/
COPY --from=builder /app/build/src/pg-status /app/pg-status

ENTRYPOINT ["/app/pg-status"]
