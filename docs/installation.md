# Installation

You can currently set up and run the project in the following ways:

## Install deb package

You can download a deb package for Linux amd64 from [releases](https://github.com/krylosov-aa/pg-status/releases/).

[Latest deb package](https://github.com/krylosov-aa/pg-status/releases/download/2.1.1/pg-status_2.1.1_amd64.deb)

```shell
wget https://github.com/krylosov-aa/pg-status/releases/download/2.1.1/pg-status_2.1.1_amd64.deb && \
sudo apt install ./pg-status_2.1.1_amd64.deb
```

then run:
```shell
pg-status
```


## Run a Docker container

There are several available options:

### Docker Hub
- [Fast build, very lightweight container](https://hub.docker.com/r/krylosovaa/pg-status)

### Alpine
- [Fast build, very lightweight container](../docker/alpine/Dockerfile_shared)
- [The heaviest among the lightweight options, but provides a static binary](../docker/alpine/Dockerfile_static)

### Ubuntu
- [With dynamic linking](../docker/ubuntu/Dockerfile_shared)
- [Static binary](../docker/ubuntu/Dockerfile_static)


## Static binary

A statically linked binary is provided — you can simply download it and run it without any additional setup.
You can download pre-built Linux amd64 binaries from [releases](https://github.com/krylosov-aa/pg-status/releases/).

[Latest static binary](https://github.com/krylosov-aa/pg-status/releases/download/2.1.1/pg-status_2.1.1_linux_amd64_static.tar.gz)

```shell
wget -qO- https://github.com/krylosov-aa/pg-status/releases/download/2.1.1/pg-status_2.1.1_linux_amd64_static.tar.gz | tar -xzv && \
chmod +x pg-status
```

then run:
```shell
./pg-status
```


## Shared binary

A dynamically linked binary is available, which requires the necessary dependencies to be installed on your system.
You can read about the required dependencies below, and also take a look at the Dockerfiles, which demonstrate the installation.
[Latest shared binary](https://github.com/krylosov-aa/pg-status/releases/download/2.1.1/pg-status_2.1.1_linux_amd64_shared.tar.gz)

```shell
sudo apt-get install -y libpq5 libevent-2.1-7t64 libcjson1 && \
wget -qO- https://github.com/krylosov-aa/pg-status/releases/download/2.1.1/pg-status_2.1.1_linux_amd64_shared.tar.gz | tar -xzv && \
chmod +x pg-status
```

then run:
```shell
./pg-status
```


## Build with CMake

The source build supports POSIX systems with CMake 3.22 or newer and a GCC,
Clang, or AppleClang compiler. Install the development packages for libevent,
libpq, cJSON, pthreads, and pkg-config, then run:

```shell
cmake --preset release
cmake --build --preset release
cmake --install cmake-builds/release --prefix /usr/local
```

For a custom build directory, the equivalent commands are:

```shell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix /usr/local
```

Set `PG_STATUS_STATIC_DEPENDENCIES=ON` when all third-party static archives are
available and must be selected. The static [Dockerfiles](../docker) contain
complete Linux examples, including libpq's private link dependencies. The
Ubuntu/glibc static build also sets `PG_STATUS_LINK_WARNINGS_AS_ERRORS=OFF`
because glibc emits unavoidable NSS/DNS static-link warnings; other builds
keep linker warnings fatal by default.


## Dependencies

This project depends on three external libraries:
- [libevent](https://libevent.org/) — [BSD-3-Clause](https://github.com/libevent/libevent/blob/master/LICENSE)
- [libpq](https://www.postgresql.org/docs/current/libpq.html) — [PostgreSQL License](https://www.postgresql.org/about/licence/)
- [cJSON](https://github.com/DaveGamble/cJSON) — [MIT License](https://github.com/DaveGamble/cJSON/blob/master/LICENSE)



## Makefile

The [Makefile](../Makefile) contains several ready-to-use commands that you can either run directly or use as a reference.
Each Dockerfile describes a build process (which you can adapt if you’re not using these files) that allows you to
build a binary and either export it to the host or run it directly inside the container.
