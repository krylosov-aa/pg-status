.DEFAULT_GOAL := help
# Top-level workflows can share a build tree; compilation itself stays parallel.
.NOTPARALLEL:

# Local CMake builds and test profiles.
CMAKE_BUILDS_DIR ?= cmake-builds
DEBUG_BUILD_DIR ?= $(CMAKE_BUILDS_DIR)/debug
RELEASE_BUILD_DIR ?= $(CMAKE_BUILDS_DIR)/release
ASAN_BUILD_DIR ?= $(CMAKE_BUILDS_DIR)/asan
TSAN_BUILD_DIR ?= $(CMAKE_BUILDS_DIR)/tsan
SCAN_BUILD_DIR ?= $(CMAKE_BUILDS_DIR)/scan
VALGRIND_BUILD_DIR ?= $(CMAKE_BUILDS_DIR)/valgrind
SCAN_REPORT_DIR ?= scan_reports
REPEAT_COUNT ?= 100
PRE_RELEASE_TSAN ?= 1
PRE_RELEASE_VALGRIND ?= 1
PRE_RELEASE_DOCKERFILE ?= test/pre-release/Dockerfile
PRE_RELEASE_IMAGE ?= pg-status-pre-release
PRE_RELEASE_PLATFORM ?= $(shell docker version --format '{{.Server.Os}}/{{.Server.Arch}}')
PRE_RELEASE_CONTAINER_LABEL ?= com.pg-status.role=pre-release
ARTIFACT_DIR ?= out
RELEASE_PLATFORM ?= linux/amd64
RELEASE_DOCKER_BUILD_FLAGS ?=
FULL_AUDIT_PULL ?= 1
FULL_AUDIT_NO_CACHE ?= 1
FULL_AUDIT_EMULATED_REPEAT_COUNT ?= 1
UV ?= uv
CTEST_ARGS ?=
PYTHON_E2E_DIR ?= $(CURDIR)/test/e2e
UV_CACHE_DIR ?= $(PYTHON_E2E_DIR)/.uv-cache
UV_RUN = UV_CACHE_DIR="$(UV_CACHE_DIR)" $(UV) run \
	--directory "$(PYTHON_E2E_DIR)" --frozen

HOST_OS := $(shell uname -s)
SYSTEM_CC = $(shell command -v clang 2>/dev/null || command -v cc 2>/dev/null)
SYSTEM_SCAN_BUILD = $(shell command -v scan-build 2>/dev/null)
SYSTEM_VALGRIND = $(shell command -v valgrind 2>/dev/null)
SYSTEM_SCAN_ANALYZER_CC = $(shell \
	analyzer="$$(command -v ccc-analyzer 2>/dev/null)"; \
	if [ -n "$$analyzer" ]; then \
		printf '%s' "$$analyzer"; \
	elif [ -n "$(SYSTEM_SCAN_BUILD)" ]; then \
		scan_build_real="$$( \
			realpath "$(SYSTEM_SCAN_BUILD)" 2>/dev/null || \
			readlink -f "$(SYSTEM_SCAN_BUILD)" 2>/dev/null || \
			printf '%s' "$(SYSTEM_SCAN_BUILD)" \
		)"; \
		candidate="$$(dirname "$$(dirname "$$scan_build_real")")/libexec/ccc-analyzer"; \
		if [ -x "$$candidate" ]; then printf '%s' "$$candidate"; fi; \
	fi \
)

ifeq ($(HOST_OS),Darwin)
HOMEBREW_LLVM_PREFIX ?= $(shell \
	if command -v brew >/dev/null 2>&1; then brew --prefix llvm 2>/dev/null; fi \
)
endif

DEFAULT_CC = $(if $(HOMEBREW_LLVM_PREFIX),$(HOMEBREW_LLVM_PREFIX)/bin/clang,$(SYSTEM_CC))
DEFAULT_SCAN_BUILD = $(if $(HOMEBREW_LLVM_PREFIX),$(HOMEBREW_LLVM_PREFIX)/bin/scan-build,$(SYSTEM_SCAN_BUILD))
DEFAULT_SCAN_ANALYZER_CC = $(if $(HOMEBREW_LLVM_PREFIX),$(HOMEBREW_LLVM_PREFIX)/libexec/ccc-analyzer,$(SYSTEM_SCAN_ANALYZER_CC))

SANITIZER_CC ?= $(DEFAULT_CC)
CLANG_FORMAT ?= $(if $(HOMEBREW_LLVM_PREFIX),$(HOMEBREW_LLVM_PREFIX)/bin/clang-format,clang-format)
SCAN_BUILD_TOOL ?= $(DEFAULT_SCAN_BUILD)
SCAN_BUILD_CC ?= $(SANITIZER_CC)
SCAN_ANALYZER_CC ?= $(DEFAULT_SCAN_ANALYZER_CC)
VALGRIND_TOOL ?= $(SYSTEM_VALGRIND)
VALGRIND_ERROR_EXIT_CODE ?= 99
VALGRIND_OPTIONS ?= --tool=memcheck --leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=definite,indirect --track-origins=yes --num-callers=50 --error-exitcode=$(VALGRIND_ERROR_EXIT_CODE)

# Discover Docker Compose only for targets that use it.
COMPOSE ?= $(shell docker compose version >/dev/null 2>&1 && echo docker compose || echo docker-compose)
TEST_PROJECT ?= test
docker-compose = $(COMPOSE) -f docker-compose.yml
test-docker-compose = $(COMPOSE) -p "$(TEST_PROJECT)" -f test/docker/docker-compose.yml

.PHONY: \
	help \
	build_debug test build_release test_release \
	build_static_alpine \
	build_shared_alpine \
	build_shared_ubuntu \
	build_static_ubuntu \
	build_shared_executable \
	build_static_executable \
	build_deb \
	check_publish_args \
	build_push \
	build \
	up \
	down \
	down_all \
	build_up \
	build_up_test \
	build_up_test_only_pg \
	down_test \
	1-master \
	2-master \
	test_e2e \
	test_e2e_asan \
	test_e2e_tsan \
	test_e2e_valgrind \
	test_e2e_all \
	test_e2e_cleanup \
	python_sync \
	python_lint \
	python_fix python_check \
	check_scan_build_tools \
	check_valgrind \
	configure_scan_build \
	scan-build \
	scan-build-product \
	clean \
	clean_release \
	build_valgrind \
	test_valgrind \
	format \
	format-check \
	check_repeat_args \
	check_pre_release_args \
	build_asan \
	test_asan \
	build_tsan \
	test_tsan \
	test_repeat \
	test_repeat_asan \
	test_repeat_tsan \
	pre-release \
	pre-release-docker \
	full-audit \
	release-builds \
	benchmark_rps

help: ## Show available targets; detailed guide: docs/make-targets.md.
	@awk 'BEGIN { FS = ":.*## "; print "Usage: make <target> [VARIABLE=value]\n" } \
		/^[a-zA-Z0-9_-]+:.*## / { printf "  %-28s %s\n", $$1, $$2 }' Makefile

# Docker runtime images and distributable files.
build_static_alpine: ## Build the Alpine image with a static executable.
	docker build $(RELEASE_DOCKER_BUILD_FLAGS) \
		--platform "$(RELEASE_PLATFORM)" --load \
		-f docker/alpine/Dockerfile_static -t pg-status-static-alpine .

build_shared_alpine: ## Build the Alpine image with shared libraries.
	docker build $(RELEASE_DOCKER_BUILD_FLAGS) \
		--platform "$(RELEASE_PLATFORM)" --load \
		-f docker/alpine/Dockerfile_shared -t pg-status-shared-alpine .

build_shared_ubuntu: ## Build the Ubuntu image with shared libraries.
	docker build $(RELEASE_DOCKER_BUILD_FLAGS) \
		--platform "$(RELEASE_PLATFORM)" --load \
		-f docker/ubuntu/Dockerfile_shared -t pg-status-shared-ubuntu .

build_static_ubuntu: ## Build the Ubuntu image with a static executable.
	docker build $(RELEASE_DOCKER_BUILD_FLAGS) \
		--platform "$(RELEASE_PLATFORM)" --load \
		-f docker/ubuntu/Dockerfile_static -t pg-status-static-ubuntu .

build_shared_executable: ## Export the shared-library tar.gz to out/shared/.
	docker build $(RELEASE_DOCKER_BUILD_FLAGS) \
		--platform "$(RELEASE_PLATFORM)" \
		-f docker/ubuntu/Dockerfile_shared --target export \
		--output "type=local,dest=$(ARTIFACT_DIR)/shared" .

build_static_executable: ## Export the static tar.gz to out/static/.
	docker build $(RELEASE_DOCKER_BUILD_FLAGS) \
		--platform "$(RELEASE_PLATFORM)" \
		-f docker/ubuntu/Dockerfile_static --target export \
		--output "type=local,dest=$(ARTIFACT_DIR)/static" .

build_deb: ## Export the Ubuntu/Debian package to out/deb/.
	docker build $(RELEASE_DOCKER_BUILD_FLAGS) \
		--platform "$(RELEASE_PLATFORM)" \
		-f docker/ubuntu/Dockerfile_deb --target export \
		--output "type=local,dest=$(ARTIFACT_DIR)/deb" .

check_publish_args:
	@test -n "$(strip $(r))" || { \
		printf 'Registry/repository prefix is required: make build_push r=<registry-or-user> v=<version>\n' >&2; \
		exit 2; \
	}
	@test -n "$(strip $(v))" || { \
		printf 'Image version is required: make build_push r=<registry-or-user> v=<version>\n' >&2; \
		exit 2; \
	}

build_push: check_publish_args ## Build and PUSH version/latest tags; requires r=... v=....
	docker buildx build $(RELEASE_DOCKER_BUILD_FLAGS) \
		--platform "$(RELEASE_PLATFORM)" \
		-f docker/alpine/Dockerfile_shared \
		-t "$(r)/pg-status:$(v)" \
		-t "$(r)/pg-status:latest" \
		--push \
		.

# Manual Docker environments. Startup preserves existing PostgreSQL data.
build: ## Build the native pg-status image for the root Compose environment.
	docker build --load -f docker/alpine/Dockerfile_shared -t pg-status .

up: ## Start pg-status from the root Compose file; requires .env/database settings.
	$(docker-compose) up -d

down: ## Stop the root Compose environment.
	$(docker-compose) down --remove-orphans

down_all: ## Stop both manual Compose environments; delete the test database volumes.
	@exit_code=0; \
	$(docker-compose) down --remove-orphans || exit_code=$$?; \
	$(test-docker-compose) --profile pg-status --profile security \
		down --volumes --remove-orphans \
		|| exit_code=$$?; \
	exit "$$exit_code"

build_up: build ## Build and start the root Compose environment.
	$(MAKE) up

build_up_test: ## Build/start pg-status and PostgreSQL 18 with replicas and proxies.
	PG_STATUS_E2E_BUILD_TYPE=Release \
		PG_STATUS_E2E_SANITIZER=none \
		PG_STATUS_E2E_MODE=release \
		PG_STATUS_E2E_HTTP_PORT="$${PG_STATUS_E2E_HTTP_PORT:-8000}" \
		PG_STATUS_E2E_PROXY_1_PORT="$${PG_STATUS_E2E_PROXY_1_PORT:-5435}" \
		PG_STATUS_E2E_PROXY_2_PORT="$${PG_STATUS_E2E_PROXY_2_PORT:-5436}" \
		PG_STATUS_E2E_PROXY_3_PORT="$${PG_STATUS_E2E_PROXY_3_PORT:-5437}" \
		$(test-docker-compose) --profile pg-status up -d --build

build_up_test_only_pg: ## Start PostgreSQL and proxies for a locally running pg-status.
	PG_STATUS_E2E_PROXY_1_PORT="$${PG_STATUS_E2E_PROXY_1_PORT:-5435}" \
		PG_STATUS_E2E_PROXY_2_PORT="$${PG_STATUS_E2E_PROXY_2_PORT:-5436}" \
		PG_STATUS_E2E_PROXY_3_PORT="$${PG_STATUS_E2E_PROXY_3_PORT:-5437}" \
		$(test-docker-compose) up -d --build

test_e2e_cleanup: ## Delete one abandoned e2e project; requires E2E_PROJECT=<exact-name>.
	@test -n "$(E2E_PROJECT)" || { echo 'Specify E2E_PROJECT=<exact-project-name>' >&2; exit 2; }
	$(COMPOSE) --project-name "$(E2E_PROJECT)" \
		-f test/docker/docker-compose.yml --profile pg-status --profile security \
		down --volumes --remove-orphans

down_test: ## Stop the manual test topology and DELETE its database/certificate volumes.
	$(test-docker-compose) --profile pg-status --profile security \
		down --volumes --remove-orphans

1-master: ## Route proxy 1 to the existing primary; proxies 2/3 to replicas.
	COMPOSE="$(COMPOSE)" TEST_PROJECT="$(TEST_PROJECT)" ./test/docker/pg-proxy-1_is_master.sh

2-master: ## Route proxy 2 to the existing primary, proxy 1 to replica 1.
	COMPOSE="$(COMPOSE)" TEST_PROJECT="$(TEST_PROJECT)" ./test/docker/pg-proxy-2_is_master.sh

# Automated PostgreSQL tests and Python tooling.
test_e2e: ## Run the complete PostgreSQL 18 e2e suite in Release mode.
	$(UV_RUN) pytest tests --e2e-profile release $(E2E_PYTEST_ARGS)

test_e2e_asan: ## Run PostgreSQL e2e under ASan/UBSan.
	$(UV_RUN) pytest tests --e2e-profile asan $(E2E_PYTEST_ARGS)

test_e2e_tsan: ## Run PostgreSQL e2e under TSan.
	$(UV_RUN) pytest tests --e2e-profile tsan $(E2E_PYTEST_ARGS)

test_e2e_valgrind: ## Run PostgreSQL e2e under Valgrind.
	$(UV_RUN) pytest tests --e2e-profile valgrind $(E2E_PYTEST_ARGS)

python_check: python_lint ## Check Python formatting/lint/types and run tooling regression tests.
	$(UV_RUN) ruff format --check .
	$(UV_RUN) pytest checks $(E2E_CHECK_ARGS)
	UV_CACHE_DIR="$(UV_CACHE_DIR)" $(UV) run --project "$(PYTHON_E2E_DIR)" --frozen \
		python -m unittest discover -s test/audit/tests

test_e2e_all: ## Run all four e2e profiles sequentially, retaining any failure status.
	@status=0; \
	$(MAKE) test_e2e || status=$$?; \
	$(MAKE) test_e2e_asan || status=$$?; \
	$(MAKE) test_e2e_tsan || status=$$?; \
	$(MAKE) test_e2e_valgrind || status=$$?; \
	exit $$status

python_sync: ## Install the locked Python environment used by tests and audit.
	UV_CACHE_DIR="$(UV_CACHE_DIR)" $(UV) sync \
		--directory "$(PYTHON_E2E_DIR)" --frozen

python_lint: ## Check Python lint and types without changing files.
	$(UV_RUN) ruff check .
	$(UV_RUN) mypy .
	$(UV_RUN) ruff check --select E,F --line-length 79 "$(CURDIR)/test/audit"
	$(UV_RUN) ruff format --check --line-length 79 "$(CURDIR)/test/audit"

python_fix: ## Apply safe Ruff fixes and formatting to project Python files.
	$(UV_RUN) ruff check --fix .
	$(UV_RUN) ruff format .
	$(UV_RUN) ruff check --fix --select E,F --line-length 79 "$(CURDIR)/test/audit"
	$(UV_RUN) ruff format --line-length 79 "$(CURDIR)/test/audit"

check_scan_build_tools:
	@command -v "$(SCAN_BUILD_TOOL)" >/dev/null 2>&1 || { \
		printf 'scan-build not found: %s\n' "$(SCAN_BUILD_TOOL)" >&2; \
		exit 1; \
	}
	@command -v "$(SCAN_BUILD_CC)" >/dev/null 2>&1 || { \
		printf 'scan-build C compiler not found: %s\n' "$(SCAN_BUILD_CC)" >&2; \
		exit 1; \
	}
	@command -v "$(SCAN_ANALYZER_CC)" >/dev/null 2>&1 || { \
		printf 'ccc-analyzer not found: %s\n' "$(SCAN_ANALYZER_CC)" >&2; \
		exit 1; \
	}

configure_scan_build: check_scan_build_tools
	"$(SCAN_BUILD_TOOL)" --use-cc="$(SCAN_BUILD_CC)" \
		bash test/configure.sh "$(SCAN_ANALYZER_CC)" "$(SCAN_BUILD_DIR)" \
		-DCMAKE_BUILD_TYPE=Debug \
		-DBUILD_TESTING=ON \
		-DPG_STATUS_SANITIZER=none \
		-DPG_STATUS_ENABLE_CLANG_TIDY=OFF

scan-build-product: configure_scan_build ## Run Clang Static Analyzer on product code only.
	"$(SCAN_BUILD_TOOL)" --status-bugs --use-cc="$(SCAN_BUILD_CC)" \
		-o "$(SCAN_REPORT_DIR)" \
		cmake --build "$(SCAN_BUILD_DIR)" --target pg-status \
		--clean-first --parallel

scan-build: configure_scan_build ## Run Clang Static Analyzer on product and C test code.
	"$(SCAN_BUILD_TOOL)" --status-bugs --use-cc="$(SCAN_BUILD_CC)" \
		-o "$(SCAN_REPORT_DIR)" \
		cmake --build "$(SCAN_BUILD_DIR)" --clean-first --parallel

clean: ## Clean outputs of all configured local CMake profiles, keeping their caches.
	@set -e; for directory in "$(DEBUG_BUILD_DIR)" "$(RELEASE_BUILD_DIR)" \
		"$(ASAN_BUILD_DIR)" "$(TSAN_BUILD_DIR)" "$(SCAN_BUILD_DIR)" "$(VALGRIND_BUILD_DIR)"; do \
		if [ -f "$$directory/CMakeCache.txt" ]; then \
			cmake --build "$$directory" --target clean; \
		fi; \
	done

clean_release: ## Clean only the local Release build outputs, keeping the cache.
	@if [ -f "$(RELEASE_BUILD_DIR)/CMakeCache.txt" ]; then \
		cmake --build "$(RELEASE_BUILD_DIR)" --verbose --target clean; \
	else \
		printf 'Nothing to clean: %s is not configured\n' "$(RELEASE_BUILD_DIR)"; \
	fi

check_valgrind:
	@if [ -z "$(VALGRIND_TOOL)" ] || ! command -v "$(VALGRIND_TOOL)" >/dev/null 2>&1; then \
		printf 'Valgrind not found: %s\n' "$(VALGRIND_TOOL)" >&2; \
		exit 1; \
	fi

build_valgrind: check_valgrind ## Build RelWithDebInfo binaries configured for Valgrind.
	bash test/configure.sh "$(SANITIZER_CC)" "$(VALGRIND_BUILD_DIR)" \
		-DCMAKE_BUILD_TYPE=RelWithDebInfo \
		-DBUILD_TESTING=ON \
		-DPG_STATUS_SANITIZER=none \
		-DPG_STATUS_ENABLE_CLANG_TIDY=OFF \
		-DPG_STATUS_TEST_VALGRIND="$(VALGRIND_TOOL)" \
		-DPG_STATUS_TEST_VALGRIND_OPTIONS="$(VALGRIND_OPTIONS)" \
		-DMEMORYCHECK_TYPE=Valgrind \
		-DMEMORYCHECK_COMMAND="$(VALGRIND_TOOL)" \
		-DMEMORYCHECK_COMMAND_OPTIONS="$(VALGRIND_OPTIONS)"
	cmake --build "$(VALGRIND_BUILD_DIR)" --parallel

test_valgrind: build_valgrind ## Run CTest Memcheck, including children of expected-failure tests (Linux).
	ctest --test-dir "$(VALGRIND_BUILD_DIR)" \
		-T memcheck \
		--output-on-failure \
		--no-tests=error $(CTEST_ARGS)

# Formatting does not require configuring/building the C project.
format: FORMAT_FLAGS = -i
format: ## Rewrite C source/header formatting.
format-check: FORMAT_FLAGS = --dry-run --Werror
format-check: ## Check C formatting without modifying files; fail on differences.
format format-check:
	@command -v "$(CLANG_FORMAT)" >/dev/null 2>&1 || { \
		printf 'clang-format not found: %s\n' "$(CLANG_FORMAT)" >&2; exit 1; \
	}
	find src test -type d \( -name .venv -o -name .uv-cache \
		-o -name __pycache__ -o -name build \) -prune -o \
		-type f \( -name '*.c' -o -name '*.h' \) \
		-exec "$(CLANG_FORMAT)" $(FORMAT_FLAGS) {} +

check_repeat_args:
	@case "$(REPEAT_COUNT)" in \
		'' | *[!0-9]* | 0*) \
			printf 'REPEAT_COUNT must be a positive integer without leading zeros, got: %s\n' \
				"$(REPEAT_COUNT)" >&2; \
			exit 2; \
			;; \
	esac
	@test "$(REPEAT_COUNT)" -gt 0 2>/dev/null || { \
		printf 'REPEAT_COUNT is outside the supported integer range\n' >&2; exit 2; \
	}

check_pre_release_args:
	@case "$(PRE_RELEASE_TSAN)" in \
		0 | 1) ;; \
		*) \
			printf 'PRE_RELEASE_TSAN must be 0 or 1, got: %s\n' \
				"$(PRE_RELEASE_TSAN)" >&2; \
			exit 2; \
			;; \
	esac

	@case "$(PRE_RELEASE_VALGRIND)" in \
		0 | 1) ;; \
		*) echo 'PRE_RELEASE_VALGRIND must be 0 or 1' >&2; exit 2 ;; \
	esac

# CMake profiles. The helper handles compiler changes without losing options.
build_debug: ## Build local Debug binaries and C tests.
	bash test/configure.sh "$(SANITIZER_CC)" "$(DEBUG_BUILD_DIR)" \
		-DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
		-DPG_STATUS_SANITIZER=none -DPG_STATUS_ENABLE_CLANG_TIDY=OFF
	cmake --build "$(DEBUG_BUILD_DIR)" --parallel

test: build_debug ## Build and run the ordinary local Debug CTest suite; no Docker/PostgreSQL.
	ctest --test-dir "$(DEBUG_BUILD_DIR)" --output-on-failure \
		--no-tests=error --output-junit "$$(cd "$(DEBUG_BUILD_DIR)" && pwd)/Testing/full.xml" $(CTEST_ARGS)

build_release: ## Build local Release binaries and C tests.
	bash test/configure.sh "$(SANITIZER_CC)" "$(RELEASE_BUILD_DIR)" \
		-DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
		-DPG_STATUS_SANITIZER=none -DPG_STATUS_ENABLE_CLANG_TIDY=OFF
	cmake --build "$(RELEASE_BUILD_DIR)" --parallel

test_release: build_release ## Build and run the local Release CTest suite once.
	ctest --test-dir "$(RELEASE_BUILD_DIR)" --output-on-failure \
		--no-tests=error --output-junit "$$(cd "$(RELEASE_BUILD_DIR)" && pwd)/Testing/full.xml" $(CTEST_ARGS)

build_asan: ## Build Debug binaries with ASan/UBSan and clang-tidy.
	bash test/configure.sh "$(SANITIZER_CC)" "$(ASAN_BUILD_DIR)" \
		-DCMAKE_BUILD_TYPE=Debug \
		-DBUILD_TESTING=ON \
		-DPG_STATUS_SANITIZER=address-undefined \
		-DPG_STATUS_ENABLE_CLANG_TIDY=ON
	cmake --build "$(ASAN_BUILD_DIR)" --parallel

test_asan: build_asan ## Run local CTest with ASan/UBSan; also check clang-tidy during compilation.
	ctest --test-dir "$(ASAN_BUILD_DIR)" --output-on-failure \
		--no-tests=error --output-junit "$$(cd "$(ASAN_BUILD_DIR)" && pwd)/Testing/full.xml" $(CTEST_ARGS)

build_tsan: ## Build Debug binaries with TSan.
	bash test/configure.sh "$(SANITIZER_CC)" "$(TSAN_BUILD_DIR)" \
		-DCMAKE_BUILD_TYPE=Debug \
		-DBUILD_TESTING=ON \
		-DPG_STATUS_SANITIZER=thread \
		-DPG_STATUS_ENABLE_CLANG_TIDY=OFF
	cmake --build "$(TSAN_BUILD_DIR)" --parallel

test_tsan: build_tsan ## Run the local CTest suite with TSan.
	ctest --test-dir "$(TSAN_BUILD_DIR)" --output-on-failure \
		--no-tests=error --output-junit "$$(cd "$(TSAN_BUILD_DIR)" && pwd)/Testing/full.xml" $(CTEST_ARGS)

test_repeat: check_repeat_args test_release ## Run Release CTest once, then repeat stress tests (REPEAT_COUNT=100).
	@if [ "$(REPEAT_COUNT)" -gt 1 ]; then \
		ctest --test-dir "$(RELEASE_BUILD_DIR)" --output-on-failure \
			--no-tests=error -L stress --output-junit "$$(cd "$(RELEASE_BUILD_DIR)" && pwd)/Testing/stress.xml" \
			--repeat until-fail:$$(($(REPEAT_COUNT) - 1)) $(CTEST_ARGS); \
	fi

test_repeat_asan: check_repeat_args test_asan ## Run ASan/UBSan CTest once, then repeat stress tests.
	@if [ "$(REPEAT_COUNT)" -gt 1 ]; then \
		ctest --test-dir "$(ASAN_BUILD_DIR)" --output-on-failure \
			--no-tests=error -L stress --output-junit "$$(cd "$(ASAN_BUILD_DIR)" && pwd)/Testing/stress.xml" \
			--repeat until-fail:$$(($(REPEAT_COUNT) - 1)) $(CTEST_ARGS); \
	fi

test_repeat_tsan: check_repeat_args test_tsan ## Run TSan CTest once, then repeat stress tests.
	@if [ "$(REPEAT_COUNT)" -gt 1 ]; then \
		ctest --test-dir "$(TSAN_BUILD_DIR)" --output-on-failure \
			--no-tests=error -L stress --output-junit "$$(cd "$(TSAN_BUILD_DIR)" && pwd)/Testing/stress.xml" \
			--repeat until-fail:$$(($(REPEAT_COUNT) - 1)) $(CTEST_ARGS); \
	fi

pre-release: check_repeat_args check_pre_release_args ## Run the local C release gate sequentially; excludes PostgreSQL e2e/packaging.
	$(MAKE) format-check
	$(MAKE) scan-build
	$(MAKE) REPEAT_COUNT=$(REPEAT_COUNT) test_repeat_asan
ifeq ($(PRE_RELEASE_TSAN),1)
	$(MAKE) REPEAT_COUNT=$(REPEAT_COUNT) test_repeat_tsan
else
	@printf 'Skipping TSan: PRE_RELEASE_TSAN=0\n'
endif
	$(MAKE) REPEAT_COUNT=$(REPEAT_COUNT) test_repeat
ifeq ($(HOST_OS),Linux)
ifeq ($(PRE_RELEASE_VALGRIND),1)
	$(MAKE) test_valgrind
else
	@printf 'Skipping Valgrind: PRE_RELEASE_VALGRIND=0\n'
endif
else
	@printf 'Skipping Valgrind: supported by the Linux pre-release gate\n'
endif

pre-release-docker: check_repeat_args check_pre_release_args ## Run the C gate in native Linux Docker; retain reports under out/pre-release-*.
	@set -e; \
	platform="$(PRE_RELEASE_PLATFORM)"; \
	native="$$(docker version --format '{{.Server.Os}}/{{.Server.Arch}}')"; \
	case "$$platform" in linux/amd64|linux/arm64) ;; \
		*) echo 'PRE_RELEASE_PLATFORM must be linux/amd64 or linux/arm64' >&2; exit 2 ;; esac; \
	if [ "$$platform" != "$$native" ] && \
		{ [ "$(PRE_RELEASE_TSAN)" = 1 ] || [ "$(PRE_RELEASE_VALGRIND)" = 1 ]; }; then \
		echo 'TSan/Valgrind require the native Docker platform; use full-audit to check both architectures' >&2; \
		exit 2; \
	fi; \
	mkdir -p "$(ARTIFACT_DIR)"; \
	artifacts="$$(cd "$(ARTIFACT_DIR)" && pwd)"; \
	reports="$$(mktemp -d "$$artifacts/pre-release-XXXXXXXX")"; \
	printf 'Pre-release reports: %s\n' "$$reports"; \
	docker build --platform "$$platform" --load \
		-f "$(PRE_RELEASE_DOCKERFILE)" -t "$(PRE_RELEASE_IMAGE)" \
		--iidfile "$$reports/image.id" .; \
	docker run --rm --platform "$$platform" \
		--security-opt seccomp=unconfined \
		--label "$(PRE_RELEASE_CONTAINER_LABEL)" \
		--volume "$$reports:/reports" "$$(cat "$$reports/image.id")" \
		bash /usr/local/bin/run-gates \
			REPEAT_COUNT="$(REPEAT_COUNT)" \
			PRE_RELEASE_TSAN="$(PRE_RELEASE_TSAN)" \
			PRE_RELEASE_VALGRIND="$(PRE_RELEASE_VALGRIND)" pre-release

full-audit: ## Run the full release audit, including PostgreSQL 16/17/18 and every distributable.
	AUDIT_PLATFORM="$(RELEASE_PLATFORM)" \
	AUDIT_REPEAT_COUNT="$(REPEAT_COUNT)" \
	AUDIT_ARTIFACT_ROOT="$(ARTIFACT_DIR)" \
	AUDIT_PULL="$(FULL_AUDIT_PULL)" \
	AUDIT_NO_CACHE="$(FULL_AUDIT_NO_CACHE)" \
	AUDIT_EMULATED_REPEAT_COUNT="$(FULL_AUDIT_EMULATED_REPEAT_COUNT)" \
	UV_CACHE_DIR="$(UV_CACHE_DIR)" \
		$(UV) run --project "$(PYTHON_E2E_DIR)" --frozen \
			python "$(CURDIR)/test/audit/run.py"

release-builds: ## Build four runtime images and export three distributables; no full audit.
	$(MAKE) build_shared_alpine
	$(MAKE) build_static_alpine
	$(MAKE) build_shared_ubuntu
	$(MAKE) build_static_ubuntu
	$(MAKE) build_deb
	$(MAKE) build_shared_executable
	$(MAKE) build_static_executable

benchmark_rps: ## Load-test an already running server; see test/rps/README.md.
	./test/rps/run.sh
