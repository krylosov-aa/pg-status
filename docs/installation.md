# Installation

You can currently set up and run the project in the following ways:

## Install deb package

You can download a deb package for linux amd64 from [releases](https://github.com/krylosov-aa/pg-status/releases/).

[Latest deb package](https://github.com/krylosov-aa/pg-status/releases/download/1.4.0/pg-status_1.4.0_amd64.deb)

```shell
wget https://github.com/krylosov-aa/pg-status/releases/download/1.4.0/pg-status_1.4.0_amd64.deb && sudo dpkg -i pg-status_1.4.0_amd64.deb
```

then run:
```shell
pg-status
```

## Static binary

A statically linked binary is provided you can simply download it and run it without any additional setup.
You can download pre-built linux amd64 binaries from [releases](https://github.com/krylosov-aa/pg-status/releases/).

[Latest static binary](https://github.com/krylosov-aa/pg-status/releases/download/1.4.0/pg-status_1.4.0_amd64_static)

```shell
wget https://github.com/krylosov-aa/pg-status/releases/download/1.4.0/pg-status_1.4.0_amd64_static && chmod +x pg-status_1.4.0_amd64_static
```

## Shared binary

A dynamically linked binary is available, which requires the necessary dependencies to be installed on your system.
You can read about the required dependencies below, and also take a very look at the docker files,
which demonstrate the installation. [Latest shared binary](https://github.com/krylosov-aa/pg-status/releases/download/1.4.0/pg-status_1.4.0_amd64_shared)

```shell
sudo apt-get install libpq5 libmicrohttpd12 libcjson1 && wget https://github.com/krylosov-aa/pg-status/releases/download/1.4.0/pg-status_1.4.0_amd64_shared && chmod +x pg-status_1.4.0_amd64_shared
```

## Run a Docker container

There are several available options:

### docker-hub:
- [Fast build, very lightweight container](https://hub.docker.com/r/krylosovaa/pg-status)

### Alpine
- [Fast build, very lightweight container](docker/alpine/Dockerfile_shared)
- [The lightest container, but takes slightly longer to build](docker/alpine/Dockerfile_shared_disabled_https)
- [The heaviest among the lightweight options, but provides a static binary](docker/alpine/Dockerfile_static)

### Ubuntu
- [With dynamic linking](docker/ubuntu/Dockerfile_shared)
- [Static binary](docker/ubuntu/Dockerfile_static)

The [Makefile](Makefile) contains several ready-to-use commands that you can either run directly or use as a reference.
Each Dockerfile describes a build process (which you can adapt if you’re not using these files) that allows you to
build a binary and either export it to the host or run it directly inside the container.


## Build with CMake

You can compile the project from source for any platform using CMake.
You can refer to the Dockerfiles for examples of how to install dependencies and configure the build,
depending on whether you prefer a dynamically linked or static binary.

## Dependencies

This project depends on three external libraries:
- [libmicrohttpd](https://www.gnu.org/software/libmicrohttpd/) under [GNU LGPL v2.1](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html)
- [postgresql libpq](https://www.postgresql.org/docs/current/libpq.html)
- [CJson](https://github.com/DaveGamble/cJSON)
