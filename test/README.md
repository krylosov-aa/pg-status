# Testing pg-status

Run the commands in this document from the repository root.
For a concise guide to every Make target and when to use it, see the
[developer Make reference](../docs/make-targets.md).

## Automated tests

Configure and build the project, then run the CTest suite:

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

The `debug` and `release` presets are ordinary builds. The `asan` and `tsan`
presets enable their respective sanitizers; `asan` also enables clang-tidy.
The functional HTTP API tests are deterministic and do not require PostgreSQL
or Docker. Startup tests hold real monitor connections on local TCP listeners
to verify liveness, readiness, HTTP 503 responses during warmup, and shutdown
before the initial polls finish.

All local Make targets place their CMake build trees under `cmake-builds/` by
default. Override the common root with `CMAKE_BUILDS_DIR=<path>` or override
an individual build directory with its corresponding variable.

`make format` explicitly rewrites C formatting. Verification commands use
`format-check`, which fails without changing source files. `make python_check`
also checks formatting, lint, types and runner regression tests without fixes.

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

Run the complete suite once, then repeat tests labelled `stress` until the
first failure, using the optimized Release configuration:

```sh
make test_repeat
```

The same repeated suite can be run under the sanitizer configurations:

```sh
make test_repeat_asan
make test_repeat_tsan
```

Repeated Release builds reuse `cmake-builds/release`, overridable with
`RELEASE_BUILD_DIR`. `REPEAT_COUNT` is the total execution count for stress
tests, including their first execution in the complete suite. Repetitions
cover snapshot publication, concurrent selection, logger races/backpressure,
and startup/shutdown. Deterministic contracts run once in each profile.

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

Run the same C pre-release verification inside an Ubuntu/glibc container
on the Docker engine's native architecture with:

```sh
make pre-release-docker
```

Reports survive container removal under a new `out/pre-release-*` directory,
including failures. TSan and Valgrind require the native Docker architecture;
use `full-audit` for both the release architecture and native instrumentation.

To build all container images and distributable artifacts, use the following
packaging command. It runs the Dockerfile build tests, but does not run the
pre-release or full-audit gates:

```sh
make release-builds
```

### Full audit

Use `make full-audit` before release and after changes to CMake, the test
runner, toolchains, dependencies or packaging:

```sh
make full-audit
```

The audit copies tracked and nonignored inputs, including local edits, into
one snapshot under `out/audit-<run-id>/source`. Every build and e2e run uses
that snapshot.

The release target is Linux amd64. On an ARM Docker engine, the target CTest
pass runs under emulation, while TSan, Valgrind and stress repetitions run natively.
The report records both architectures. The stages are:

1. Compose/shell validation, Python checks and audit-runner regression tests.
2. ShellCheck for audit scripts, strict formatting, Clang Static Analyzer,
   clang-tidy, complete CTest under
   ASan/UBSan, TSan, Release and Valgrind; additional stress repetitions.
   Expected-failure tests validate the exact exit and reject sanitizer
   diagnostics. Valgrind runs their child binaries too. Fault injection checks
   that the gate rejects deliberately broken programs.
3. GCC Release preset and Ninja installation checks, bounded installed-process
   shutdown, source coverage reports and bounded input/HTTP fuzzing.
4. The complete Release e2e suite on PostgreSQL **16, 17 and 18**, plus
   ASan/UBSan, TSan and Valgrind e2e on PostgreSQL 18. These are the PostgreSQL
   major versions exercised by the release audit; other versions are not
   certified by this matrix.
5. Alpine/Ubuntu shared/static builds, one fresh build per distinct variant.
   Runtime images, archives and DEB reuse the corresponding builder output.
6. A PostgreSQL 18 smoke subset against all four production images and all three
   distributables. Each archive/DEB is installed in its own clean image; static
   tests do not inherit the shared package's libraries. Tests cover version,
   actual primary/replica observations through DNS, lag, timeout/recovery,
   TLS/mTLS and invalid certificate identities. Every process is stopped and
   its final exit status and diagnostics checked.

E2e also checks connection reuse/expiry, recovery after server termination,
continued progress of healthy databases during another database's failure,
receiver-statistics permission fallback on a real replica, and bounded FD/RSS
under mixed HTTP/polling/fault load. The load check uses generous resource
bounds; it does not assert machine-dependent throughput or p99 latency.

`report.json` stores stage status/duration, test reports, image IDs, source
identity and artifact SHA-256 values. Stage logs, CTest reports, scan-build
reports, coverage HTML/JSON, fuzz corpus/crashes and e2e teardown logs survive
both success and failure. Conditional IPv6 checks are reported explicitly.
Coverage is diagnostic; no arbitrary 100% threshold is imposed.

Each audit owns uniquely named/labeled Compose resources and image tags.
Cleanup removes only its resources, including after failures/timeouts. It
never invokes a global prune or removes projects by a shared name prefix.
The source snapshot, reports and release files remain in the artifact folder.

Configuration examples:

```sh
# Shorter diagnostic pass; still runs all stages and runtime profiles.
make full-audit REPEAT_COUNT=5 FULL_AUDIT_PULL=0 FULL_AUDIT_NO_CACHE=0

# Explicit reduced PostgreSQL matrix for diagnosis (reported in the results).
AUDIT_POSTGRES_VERSIONS="18" make full-audit
```

Defaults: `REPEAT_COUNT=100`, `FULL_AUDIT_PULL=1`,
`FULL_AUDIT_NO_CACHE=1`, `FULL_AUDIT_EMULATED_REPEAT_COUNT=1`,
`AUDIT_POSTGRES_VERSIONS="16 17 18"`, `AUDIT_BUILD_TIMEOUT=3600`,
`AUDIT_TEST_TIMEOUT=3600`, `PG_STATUS_E2E_SOAK_SECONDS=30`.
`AUDIT_POSTGRES_VERSIONS` controls only the Release compatibility matrix.
Sanitizer, Valgrind and release-artifact checks always use PostgreSQL 18,
recorded separately as `runtime_postgres_version` in the report.
`ARTIFACT_DIR` controls the output root; `AUDIT_ARTIFACT_DIR` selects a new,
exact output directory. The directory must not already exist. Build/test
stage timeouts are seconds. Individual e2e commands and HTTP requests also
have deadlines; timeouts fail the audit and trigger cleanup.


## Docker environments

### Automated PostgreSQL e2e tests

The e2e suite starts a real PostgreSQL 18 primary, two physical streaming
replicas, three HAProxy endpoints, and an instrumented pg-status container.

Tests live only in [`e2e/tests`](e2e/tests); Docker, PostgreSQL, HAProxy,
and HTTP support code lives separately in [`e2e/support`](e2e/support). Test
dependencies are provided through pytest fixtures.

The suite is separate from CTest:

```sh
make test_e2e
make test_e2e_asan
make test_e2e_tsan
make test_e2e_valgrind
```

Run every profile sequentially with `make test_e2e_all`. Each pytest session
owns its topology and cleans up even when setup fails. To remove an abandoned
project explicitly, use `make test_e2e_cleanup E2E_PROJECT=<exact-name>`.
Automatic cleanup never enumerates all `pg-status-e2e-*` projects.

PostgreSQL 18 is the default for all e2e profiles and the manual Docker topology.
`PG_STATUS_POSTGRES_VERSION` can override it for an explicit compatibility run.
`PG_STATUS_E2E_IMAGE=<image>` exercises an existing production image
without rebuilding pg-status; `PG_STATUS_E2E_PLATFORM=linux/amd64` selects its
architecture. Infrastructure remains native to the Docker engine.

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

This builds the e2e Release image and starts the full environment defined in
[docker/docker-compose.yml](docker/docker-compose.yml).
Existing database volumes are preserved. To reset the database state explicitly,
run `make down_test` before starting the topology again.

The environment contains pg-status, one PostgreSQL primary, two physical
replicas, and three proxy services. Switching a proxy's target simulates a
role change or disconnection without stopping PostgreSQL. The manual target
publishes pg-status on port 8000 and the proxies on ports 5435 through 5437.

### Run only the PostgreSQL topology

To start the primary, two replicas, and three proxy services without pg-status,
use:

```sh
make build_up_test_only_pg
```

You can then run a locally built pg-status instance against the proxy ports
exposed by Docker.

Use these helper scripts to change the proxy configuration:

- [docker/pg-proxy-1_is_master.sh](docker/pg-proxy-1_is_master.sh)
- [docker/pg-proxy-2_is_master.sh](docker/pg-proxy-2_is_master.sh)

For individual proxy operations, use the routing helper directly, for example:

```sh
test/docker/proxy-route.sh pg-proxy-3 none
test/docker/proxy-route.sh pg-proxy-3 replica_2
```

Stop either test topology with:

```sh
make down_test
```

To stop both manual Compose environments and remove the test database volumes,
run:

```sh
make down_all
```

This does not stop independent e2e sessions, pre-release containers or audits.
`TEST_PROJECT` selects the manual test project (default `test`) consistently
for startup, proxy routing and cleanup.

## Sustainable-RPS benchmark

The manual benchmark is intentionally separate from CTest because its result
depends on the host and runtime environment. See the
[benchmark documentation](rps/README.md) for prerequisites, workload, SLOs,
and commands.
