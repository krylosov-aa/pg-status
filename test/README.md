# Testing pg-status

Run the commands in this document from the repository root.

## Automated tests

Configure and build the project, then run the CTest suite:

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

The `debug` and `release` presets are ordinary builds. The `asan` and `tsan`
presets explicitly enable their respective sanitizer and clang-tidy profiles.
The functional HTTP API tests are deterministic and do not require PostgreSQL
or Docker.

All local Make targets place their CMake build trees under `cmake-builds/` by
default. Override the common root with `CMAKE_BUILDS_DIR=<path>` or override
an individual build directory with its corresponding variable.

`make pre-release` invokes this target before analysis and tests, so a local
pre-release run may update files in the working tree. The containerized
`pre-release-docker` and `full-audit` variants format only their isolated
source copies.

### AddressSanitizer and UndefinedBehaviorSanitizer

Run the test suite in an explicit AddressSanitizer and
UndefinedBehaviorSanitizer configuration with:

```sh
make test_asan
```

The equivalent direct CMake workflow is `cmake --preset asan`, followed by
`cmake --build --preset asan` and `ctest --preset asan`.

This configuration uses the separate `cmake-builds/asan` directory. Override
it when needed with `ASAN_BUILD_DIR=<path> make test_asan`. On macOS, the
sanitizer Make targets use Homebrew LLVM when it is installed. On other
systems, or when Homebrew LLVM is unavailable, they use `clang` from `PATH`
and fall back to the standard `cc`. Override the compiler when needed with
`SANITIZER_CC=<path> make test_asan`.

### ThreadSanitizer

Run the test suite under ThreadSanitizer in a separate build directory:

```sh
make test_tsan
```

The equivalent CMake preset is `tsan`.

The separate `cmake-builds/tsan` directory prevents ThreadSanitizer from being
combined with the incompatible AddressSanitizer configuration. Override the
directory when needed with `TSAN_BUILD_DIR=<path> make test_tsan`.

### Repeated tests

To run tests repeatedly until the first failure, use the optimized Release
configuration:

```sh
make test_repeat
```

The same repeated suite can be run under the sanitizer configurations:

```sh
make test_repeat_asan
make test_repeat_tsan
```

Repeated Release builds use the separate `cmake-builds/repeat` directory. It
can be overridden with `REPEAT_BUILD_DIR=<path>`.

### Valgrind Memcheck

On Linux, run every CTest scenario through Valgrind Memcheck with:

```sh
make test_valgrind
```

The target uses a separate `cmake-builds/valgrind` tree configured as
`RelWithDebInfo` without compiler sanitizers. Invalid memory access,
uninitialized-value use, invalid deallocation, and definitely or indirectly
lost memory fail the test and return a non-zero status. All leak categories
remain visible in the generated CTest MemCheck reports. Override the build
directory or executable with `VALGRIND_BUILD_DIR` and `VALGRIND_TOOL`.

Valgrind is not available in the macOS pre-release run. The Linux container
used by `make pre-release-docker` installs it and runs this CTest MemCheck stage
automatically.

### Clang Static Analyzer

Run Clang Static Analyzer against the pg-status executable and its production
dependencies with:

```sh
make scan-build-product
```

Analyze the complete build, including test code, with:

```sh
make scan-build
```

Both targets perform a clean Debug build without runtime sanitizers or
clang-tidy. On macOS, they use Homebrew LLVM when it is installed. Otherwise,
they resolve `scan-build`, its compiler, and `ccc-analyzer` from the system
installation. The targets fail when `scan-build` finds a potential bug. When
findings exist, HTML reports are written under `scan_reports/`.

The analyzer uses the separate `cmake-builds/scan` directory. Override paths
or tools when needed with `SCAN_BUILD_DIR`, `SCAN_REPORT_DIR`,
`SCAN_BUILD_TOOL`, `SCAN_BUILD_CC`, and `SCAN_ANALYZER_CC`.

## Pre-release checks

Run the complete local pre-release verification with:

```sh
make pre-release
```

The target runs these stages sequentially and stops at the first failure.

Every repeated stage uses `REPEAT_COUNT`, which defaults to 100. Override it
for a shorter or longer run:

```sh
REPEAT_COUNT=20 make pre-release
```

Run the same complete pre-release verification inside an Ubuntu/glibc
`linux/amd64` container with:

```sh
make pre-release-docker
```

To run this gate and then build every supported container image and
distributable artifact, use:

```sh
make release-builds
```

Full audit. Use this procedure before a release or after changing CMake, tests, toolchain
settings, dependencies, Dockerfiles, or packaging. It is intentionally more
thorough than the normal development loop.

```sh
make full-audit
```

## Docker environments

### Run pg-status with your own PostgreSQL setup

```sh
make build_up
```

This builds the
[lightweight container](../docker/alpine/Dockerfile_shared) and starts it
using the root [Docker Compose configuration](../docker-compose.yml).

Create a `.env` file from [the provided example](../.env_example), or set the
required parameters directly in [docker-compose.yml](../docker-compose.yml).
This allows you to test pg-status with your own database setup.

### Run the complete test topology

```sh
make build_up_test
```

This builds the
[lightweight container](../docker/alpine/Dockerfile_shared) and starts the
full environment defined in [docker/docker-compose.yml](docker/docker-compose.yml).

The environment contains pg-status, two PostgreSQL instances (one master and
one replica), and three proxy services. Switching a proxy's target simulates a
role change or disconnection without stopping PostgreSQL.

### Run only the PostgreSQL topology

To start the master, replica, and three proxy services without pg-status, use:

```sh
make build_up_test_only_pg
```

You can then run a locally built pg-status instance against the proxy ports
exposed by Docker.

Use these helper scripts to change the proxy configuration:

- [docker/pg-proxy-1_is_master.sh](docker/pg-proxy-1_is_master.sh)
- [docker/pg-proxy-2_is_master.sh](docker/pg-proxy-2_is_master.sh)

Stop either test topology with:

```sh
make down_test
```

To stop every container managed by this repository, including the root
Compose environment, the test topology, and an active pre-release container,
run:

```sh
make down_all
```

## Sustainable-RPS benchmark

The manual benchmark is intentionally separate from CTest because its result
depends on the host and runtime environment. See the
[benchmark documentation](rps/README.md) for prerequisites, workload, SLOs,
and commands.
