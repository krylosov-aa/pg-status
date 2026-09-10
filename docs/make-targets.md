# Make: developer reference

Run all commands from the repository root. `make` without arguments and
`make help` display the available targets. Targets that run tests build the
required binaries automatically; there is no need to run the corresponding
`build_*` target first.
Top-level targets run sequentially to avoid cleaning and rebuilding the same
build tree concurrently; compilation itself remains parallel.

## Everyday workflows

| Situation | Command |
| --- | --- |
| Quick check after changing C code | `make test` |
| Changes to memory handling, object lifetimes or error handling | `make test_asan` |
| Changes to concurrency | `make test_repeat_tsan REPEAT_COUNT=20` |
| Changes to PostgreSQL queries, reconnection or TLS | `make test_e2e` |
| Changes to the Python test tooling or audit runner | `make python_check` |
| Check formatting before a commit | `make format-check` |
| Check C code in Linux before a release | `make pre-release-docker` |
| Prepare a release or change builds, test infrastructure, dependencies or packaging | `make full-audit` |

Local C tests require CMake, a compiler and the libraries listed in the
[build instructions](installation.md); see the separate
[macOS instructions](../test/installation_mac.md) where applicable. Sanitizers
and analyzers require the corresponding LLVM tools; Valgrind runs on Linux.
Python targets require `uv`; Python and package versions are pinned in `test/e2e`.
Docker targets require a running Docker engine, Buildx and, for test topologies,
Compose.

## Local builds and C tests

These targets run without PostgreSQL or Docker. The executable is placed in
`cmake-builds/<profile>/src/pg-status`. CTest checks the code, HTTP contract,
startup failures and process shutdown; HTTP tests open local ports.

| Target | What it does and when to use it |
| --- | --- |
| `build_debug` | Builds the Debug executable and tests for debugging, without running tests. |
| `test` | Builds Debug and runs the complete CTest suite once. The main local check. |
| `build_release` | Builds the optimized Release executable and tests for manual runs or measurements. |
| `test_release` | Runs the complete CTest suite once in Release mode. |
| `build_asan` | Builds Debug with ASan/UBSan and runs clang-tidy during compilation. |
| `test_asan` | Runs CTest with ASan/UBSan to detect memory errors and undefined behavior. |
| `build_tsan` | Builds Debug with TSan, without repeating clang-tidy. |
| `test_tsan` | Runs CTest with TSan to detect data races. |
| `build_valgrind` | Builds RelWithDebInfo without compiler sanitizers and configures Memcheck. |
| `test_valgrind` | Runs the complete CTest suite under Valgrind, including child processes of expected-failure tests. |
| `test_repeat` | Runs the complete Release suite once, then repeats tests labelled `stress`. |
| `test_repeat_asan` | Uses the same repetition scheme with ASan/UBSan. |
| `test_repeat_tsan` | Uses the same repetition scheme with TSan; useful for intermittent races and shutdown failures. |
| `scan-build-product` | Performs a clean rebuild with Clang Static Analyzer on product code only. |
| `scan-build` | Also analyzes C test code. Exits with an error when findings are reported. |
| `clean` | Cleans build outputs from all configured local profiles. Keeps CMake caches, reports and Docker resources. |
| `clean_release` | Cleans only Release build outputs. |

`REPEAT_COUNT=100` is the total number of runs for each stress test, including
its initial run in the complete suite. `REPEAT_COUNT=1` disables additional
repetitions. Ordinary deterministic tests are not repeated. Release tests and
repetitions share the `cmake-builds/release` tree.

Pass CTest arguments for focused diagnosis:

```sh
make test_asan CTEST_ARGS='-R ^logger\.'
make test_repeat_tsan REPEAT_COUNT=20 CTEST_ARGS='-R ^snapshot\.'
```

A filter limits the checks to the selected tests. Run complete release checks
without `CTEST_ARGS`. Local reports are written to `Testing/full.xml` and, after
repetitions, `Testing/stress.xml` inside the build tree. Analyzer reports are
written to `scan_reports/`.

## Formatting and Python

| Target | What it does and when to use it |
| --- | --- |
| `format` | Formats C sources and headers in `src/` and `test/` with clang-format. Does not require CMake configuration. |
| `format-check` | Checks the same formatting without modifying files; differences cause failure. |
| `python_sync` | Explicitly installs the Python environment from the lock file. Other targets normally let `uv run` synchronize it automatically. |
| `python_lint` | Runs Ruff and type checks on e2e code, plus basic Ruff rules and formatting checks on the Python audit runner. Does not modify files. |
| `python_fix` | Applies safe Ruff fixes and formatting to e2e and audit Python code. Review the resulting diff. |
| `python_check` | Runs `python_lint`, checks e2e formatting and runs regression tests for the test tooling and audit runner. Does not start PostgreSQL. |

## Automated PostgreSQL tests

Each run creates an isolated Docker Compose project and removes it after the
tests. The default is PostgreSQL **18**, with a primary, two replicas, proxies
and separate TLS test environments.

| Target | What it does and when to use it |
| --- | --- |
| `test_e2e` | Runs the complete e2e suite with a Release executable. The main PostgreSQL integration check. |
| `test_e2e_asan` | Runs the complete e2e suite with ASan/UBSan. |
| `test_e2e_tsan` | Runs the complete e2e suite with TSan. |
| `test_e2e_valgrind` | Runs the complete e2e suite under Valgrind. |
| `test_e2e_all` | Runs all four profiles sequentially. Continues after a failure but returns an error if any profile fails. |
| `test_e2e_cleanup` | Removes one abandoned project and its volumes: `make test_e2e_cleanup E2E_PROJECT=<exact-name>`. |

Select a scenario: `make test_e2e E2E_PYTEST_ARGS='-k connection_lifecycle'`.
Test another server version: `make test_e2e PG_STATUS_POSTGRES_VERSION=17`.
See the [e2e documentation](../test/README.md#automated-postgresql-e2e-tests)
for additional settings.

## Manual Docker environments

`build` builds a **Docker image**; use `build_debug` or `build_release` for a
local executable. The root Compose configuration starts only pg-status; provide
connections to your own databases through `.env`, using `.env_example` as a guide.

| Target | What it does and when to use it |
| --- | --- |
| `build` | Builds the local `pg-status:latest` image for the Docker engine's architecture. |
| `up` | Starts pg-status from the root Compose configuration. The image must already be built. |
| `build_up` | Builds the image and starts or updates the root environment. |
| `down` | Stops and removes containers from the root Compose environment. |
| `build_up_test` | Builds and starts the manual topology with pg-status, PostgreSQL 18, replicas and proxies. HTTP uses port 8000; database proxies use ports 5435–5437. |
| `build_up_test_only_pg` | Starts databases and proxies for a locally running pg-status. Does not start pg-status or stop an existing pg-status container. |
| `1-master` | Routes proxy 1 to the primary and proxies 2 and 3 to replicas. Restores the default routing. |
| `2-master` | Routes proxy 2 to the same primary and proxy 1 to a replica. Simulates a role change behind an address; does not promote a PostgreSQL server. |
| `down_test` | Removes the manual test topology **including database data and certificates**. Use for a full reset. |
| `down_all` | Runs `down` and `down_test`; does not stop independent e2e sessions, audits or pre-release checks. |
| `benchmark_rps` | Uses Vegeta to load an already running pg-status, checks configured thresholds and saves results. See the [benchmark documentation](../test/rps/README.md) for conditions and parameters. |

Starting a manual topology preserves existing data. To replace an old PostgreSQL
16/17 test volume with PostgreSQL 18, first run `make down_test` if the test data
is no longer needed, then run `make build_up_test`.
`TEST_PROJECT` selects the manual project name (default: `test`); use the same
value for startup, proxy routing and shutdown.
Override ports with `PG_STATUS_E2E_HTTP_PORT` and
`PG_STATUS_E2E_PROXY_1_PORT` / `PG_STATUS_E2E_PROXY_2_PORT` /
`PG_STATUS_E2E_PROXY_3_PORT`; a value of `0` selects an available port.

## Pre-release checks

| Target | What it does and when to use it |
| --- | --- |
| `pre-release` | Sequentially checks C formatting, Clang Static Analyzer, ASan/UBSan with clang-tidy, TSan, Release and stress repetitions; also runs Valgrind on Linux. Stops at the first failure. |
| `pre-release-docker` | Runs the C checks in Ubuntu on the Docker engine's native architecture. Retains reports in a new `out/pre-release-*` directory, including failed runs. |
| `full-audit` | Audits a source snapshot: C/Python checks, GCC/installation, coverage, fuzzing, PostgreSQL e2e and all release images and packages. Results and logs are written to `out/audit-*`. |

`pre-release` and `pre-release-docker` do not run PostgreSQL e2e or release-artifact
checks. `full-audit` retains the Release compatibility matrix for
**PostgreSQL 16/17/18**; sanitizer, Valgrind and all seven release-variant checks
use **18**. The release target is `linux/amd64`; on ARM, the audit also runs checks
that require native execution in an ARM container.

Examples of shorter diagnostic runs:

```sh
make pre-release-docker REPEAT_COUNT=5
make full-audit REPEAT_COUNT=5 FULL_AUDIT_PULL=0 FULL_AUDIT_NO_CACHE=0
```

By default, `full-audit` uses `REPEAT_COUNT=100`, pulls updated base images and
rebuilds the audited variants without the cache. See the
[audit documentation](../test/README.md#full-audit) for the complete scope and
configuration.

## Building and publishing releases

These targets build for `linux/amd64` by default. They run the C tests defined
in the Dockerfiles, but **do not run the full audit**.

| Target | Output and use |
| --- | --- |
| `build_shared_alpine` | Local `pg-status-shared-alpine` image with dynamic linking. |
| `build_static_alpine` | Local `pg-status-static-alpine` image with a static executable. |
| `build_shared_ubuntu` | Local `pg-status-shared-ubuntu` image with dynamic linking. |
| `build_static_ubuntu` | Local `pg-status-static-ubuntu` image with a static executable. |
| `build_shared_executable` | Archive with a dynamically linked executable in `out/shared/`; the required libraries must be installed on the destination system. |
| `build_static_executable` | Archive with a static executable and licenses in `out/static/`. |
| `build_deb` | DEB package in `out/deb/`. |
| `release-builds` | Sequentially builds the four images above and exports three packages. Use to obtain release files without a full audit. |
| `build_push` | Builds Alpine/shared and **publishes** `<r>/pg-status:<v>` and `<r>/pg-status:latest`. Example: `make build_push r=my-registry/my-team v=2.3.0`. |

`build_push` does not verify that an audit has already run; use it on source code
that has passed the required checks. Other targets in this table do not publish
anything. Archives are exported directly from Docker builds. The package
provides the architecture in the filename; `RELEASE_ARCH` is no longer needed.

## Common settings

| Variable | Purpose |
| --- | --- |
| `CMAKE_BUILDS_DIR` | Root of local build trees; defaults to `cmake-builds`. |
| `DEBUG_BUILD_DIR`, `RELEASE_BUILD_DIR`, `ASAN_BUILD_DIR`, `TSAN_BUILD_DIR`, `VALGRIND_BUILD_DIR`, `SCAN_BUILD_DIR` | Path for an individual profile. Use separate directories for different profiles. |
| `SANITIZER_CC` | Compiler for local Make builds; prefers Homebrew LLVM on macOS, otherwise `clang`/`cc`. |
| `CLANG_FORMAT` | Formatter executable. The Docker check pins version 22.1.8. |
| `SCAN_BUILD_TOOL`, `SCAN_BUILD_CC`, `SCAN_ANALYZER_CC`, `SCAN_REPORT_DIR` | Static analyzer tools and report directory. |
| `VALGRIND_TOOL`, `VALGRIND_OPTIONS` | Valgrind executable and arguments. |
| `REPEAT_COUNT` | Number of stress-test runs; a positive integer without leading zeros. |
| `CTEST_ARGS`, `E2E_PYTEST_ARGS`, `E2E_CHECK_ARGS` | Additional arguments for local CTest, PostgreSQL e2e and e2e-tooling regression tests, respectively. |
| `ARTIFACT_DIR` | Root of artifacts and Docker reports; defaults to `out`. |
| `RELEASE_PLATFORM` | Release build architecture; defaults to `linux/amd64`. `full-audit` supports only this release target. |
| `RELEASE_DOCKER_BUILD_FLAGS` | Additional flags for release image and package builds, such as `--pull --no-cache`. |
| `PRE_RELEASE_PLATFORM`, `PRE_RELEASE_IMAGE` | Architecture and image tag for `pre-release-docker`; default to the Docker engine's native architecture and `pg-status-pre-release`. |
| `PRE_RELEASE_TSAN`, `PRE_RELEASE_VALGRIND` | Set to `0` to explicitly disable the corresponding C-check stage; both default to `1`. |
| `TEST_PROJECT`, `COMPOSE` | Manual test project name and Compose command. |
| `UV`, `PYTHON_E2E_DIR`, `UV_CACHE_DIR` | The `uv` command, Python project and cache directory. |

The helper targets `check_publish_args`, `check_repeat_args`,
`check_pre_release_args`, `check_scan_build_tools`, `check_valgrind` and
`configure_scan_build` run automatically. They validate arguments, check tool
availability or prepare the analyzer build tree. They do not need to be invoked
separately during normal development.
