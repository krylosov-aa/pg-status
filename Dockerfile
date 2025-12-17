# -------- Build --------
FROM alpine AS builder

RUN apk add --no-cache \
        build-base \
        cmake \
        pkgconfig \
        libpq-dev \
        libmicrohttpd-dev

WORKDIR /app

COPY CMakeLists.txt .
COPY src/ src/

RUN cmake -B build -DCMAKE_BUILD_TYPE=Release . && \
    cmake --build build --config Release

# -------- Run --------
FROM alpine:3.20

RUN apk add --no-cache \
        libpq \
        libmicrohttpd

WORKDIR /app

COPY --from=builder /app/build/src/main /app/main

ENTRYPOINT ["/app/main"]
