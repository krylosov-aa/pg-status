CMAKE_BUILDS_DIR ?= cmake-builds
DEBUG_BUILD_DIR ?= $(CMAKE_BUILDS_DIR)/debug
RELEASE_BUILD_DIR ?= $(CMAKE_BUILDS_DIR)/release
ASAN_BUILD_DIR ?= $(CMAKE_BUILDS_DIR)/asan
TSAN_BUILD_DIR ?= $(CMAKE_BUILDS_DIR)/tsan
REPEAT_BUILD_DIR ?= $(CMAKE_BUILDS_DIR)/repeat
SCAN_BUILD_DIR ?= $(CMAKE_BUILDS_DIR)/scan
VALGRIND_BUILD_DIR ?= $(CMAKE_BUILDS_DIR)/valgrind
SCAN_REPORT_DIR ?= scan_reports
REPEAT_COUNT ?= 100
PRE_RELEASE_DOCKERFILE ?= test/pre-release/Dockerfile
PRE_RELEASE_IMAGE ?= pg-status-pre-release
PRE_RELEASE_PLATFORM ?= linux/amd64
PRE_RELEASE_CONTAINER_LABEL ?= com.pg-status.role=pre-release

HOST_OS := $(shell uname -s)
SYSTEM_CC := $(shell command -v clang 2>/dev/null || command -v cc 2>/dev/null)
SYSTEM_SCAN_BUILD := $(shell command -v scan-build 2>/dev/null)
SYSTEM_VALGRIND := $(shell command -v valgrind 2>/dev/null)
SYSTEM_SCAN_ANALYZER_CC := $(shell \
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
ifneq ($(strip $(HOMEBREW_LLVM_PREFIX)),)
USE_HOMEBREW_LLVM := 1
endif
endif

ifeq ($(USE_HOMEBREW_LLVM),1)
DEFAULT_CC := $(HOMEBREW_LLVM_PREFIX)/bin/clang
DEFAULT_SCAN_BUILD := $(HOMEBREW_LLVM_PREFIX)/bin/scan-build
DEFAULT_SCAN_ANALYZER_CC := $(HOMEBREW_LLVM_PREFIX)/libexec/ccc-analyzer
else
DEFAULT_CC := $(SYSTEM_CC)
DEFAULT_SCAN_BUILD := $(SYSTEM_SCAN_BUILD)
DEFAULT_SCAN_ANALYZER_CC := $(SYSTEM_SCAN_ANALYZER_CC)
endif

SANITIZER_CC ?= $(DEFAULT_CC)
SCAN_BUILD_TOOL ?= $(DEFAULT_SCAN_BUILD)
SCAN_BUILD_CC ?= $(SANITIZER_CC)
SCAN_ANALYZER_CC ?= $(DEFAULT_SCAN_ANALYZER_CC)
VALGRIND_TOOL ?= $(SYSTEM_VALGRIND)
VALGRIND_ERROR_EXIT_CODE ?= 99
VALGRIND_OPTIONS ?= --tool=memcheck --leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=definite,indirect --track-origins=yes --num-callers=50 --error-exitcode=$(VALGRIND_ERROR_EXIT_CODE)

.PHONY: \
	build_static_alpine \
	build_shared_alpine \
	build_shared_ubuntu \
	build_static_ubuntu \
	build_shared_executable \
	build_static_executable \
	build_deb \
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
	check_compiler \
	check_scan_build_tools \
	check_valgrind \
	configure_scan_build \
	scan-build \
	scan-build-product \
	clean \
	clean_release \
	build_valgrind \
	test_valgrind \
	colima_start \
	build_push_amd_64 \
	format-warn \
	build_asan \
	test_asan \
	build_tsan \
	test_tsan \
	build_repeat \
	test_repeat \
	test_repeat_asan \
	test_repeat_tsan \
	pre-release \
	pre-release-docker \
	release-builds \
	benchmark_rps

build_static_alpine:
	docker build -f docker/alpine/Dockerfile_static -t pg-status-static-alpine .

build_shared_alpine:
	docker build -f docker/alpine/Dockerfile_shared -t pg-status-shared-alpine .

build_shared_ubuntu:
	docker build -f docker/ubuntu/Dockerfile_shared -t pg-status-shared-ubuntu .

build_static_ubuntu:
	docker build -f docker/ubuntu/Dockerfile_static -t pg-status-static-ubuntu .

build_shared_executable:
	sudo docker build -f docker/ubuntu/Dockerfile_shared --target export -o out/shared .
	sudo chown -R $$(id -u):$$(id -g) out/shared

build_static_executable:
	sudo docker build -f docker/ubuntu/Dockerfile_static --target export -o out/static .
	sudo chown -R $$(id -u):$$(id -g) out/static

build_deb:
	sudo docker build -f docker/ubuntu/Dockerfile_deb --target export -o out/deb .
	sudo chown -R $$(id -u):$$(id -g) out/deb

build_push:
	sudo docker buildx build \
		--platform linux/amd64 \
		--provenance=false \
		--sbom=false \
		-f docker/alpine/Dockerfile_shared \
		-t ${r}/pg-status:${v} \
		-t ${r}/pg-status:latest \
		--push .

build:
	docker build -f docker/alpine/Dockerfile_shared -t pg-status .

up:
	$(docker-compose) up -d

down:
	$(docker-compose) down --remove-orphans

down_all:
	@exit_code=0; \
	$(docker-compose) down --remove-orphans || exit_code=$$?; \
	$(test-docker-compose) --profile pg-status down --remove-orphans \
		|| exit_code=$$?; \
	pre_release_containers="$$( \
		docker ps -q --filter "label=$(PRE_RELEASE_CONTAINER_LABEL)" \
	)"; \
	docker_ps_exit=$$?; \
	if [ "$$docker_ps_exit" -ne 0 ]; then \
		exit_code=$$docker_ps_exit; \
	elif [ -n "$$pre_release_containers" ]; then \
		docker stop $$pre_release_containers || exit_code=$$?; \
	fi; \
	exit "$$exit_code"

build_up:
	make down
	make build
	make up

build_up_test:
	make down_test
	make build
	$(test-docker-compose) --profile pg-status up -d

build_up_test_only_pg:
	make down_test
	$(test-docker-compose) up -d

down_test:
	$(test-docker-compose) --profile pg-status down --remove-orphans

1-master:
	./test/docker/pg-proxy-1_is_master.sh

2-master:
	./test/docker/pg-proxy-2_is_master.sh

check_compiler:
	@command -v "$(SANITIZER_CC)" >/dev/null 2>&1 || { \
		printf 'C compiler not found: %s\n' "$(SANITIZER_CC)" >&2; \
		exit 1; \
	}

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
	$(SCAN_BUILD_TOOL) --use-cc=$(SCAN_BUILD_CC) \
		cmake -S . -B $(SCAN_BUILD_DIR) \
		-DCMAKE_C_COMPILER=$(SCAN_ANALYZER_CC)
	$(SCAN_BUILD_TOOL) --use-cc=$(SCAN_BUILD_CC) \
		cmake -S . -B $(SCAN_BUILD_DIR) \
		-DCMAKE_C_COMPILER=$(SCAN_ANALYZER_CC) \
		-DCMAKE_BUILD_TYPE=Debug \
		-DBUILD_TESTING=ON \
		-DPG_STATUS_SANITIZER=none \
		-DPG_STATUS_ENABLE_CLANG_TIDY=OFF

scan-build-product: configure_scan_build
	$(SCAN_BUILD_TOOL) --status-bugs --use-cc=$(SCAN_BUILD_CC) \
		-o $(SCAN_REPORT_DIR) \
		cmake --build $(SCAN_BUILD_DIR) --target pg-status \
		--clean-first --parallel

scan-build: configure_scan_build
	$(SCAN_BUILD_TOOL) --status-bugs --use-cc=$(SCAN_BUILD_CC) \
		-o $(SCAN_REPORT_DIR) \
		cmake --build $(SCAN_BUILD_DIR) --clean-first --parallel

clean:
	 cmake --build $(DEBUG_BUILD_DIR) --verbose --target clean

clean_release:
	 cmake --build $(RELEASE_BUILD_DIR) --verbose --target clean

check_valgrind:
	@if [ -z "$(VALGRIND_TOOL)" ] || ! command -v "$(VALGRIND_TOOL)" >/dev/null 2>&1; then \
		printf 'Valgrind not found: %s\n' "$(VALGRIND_TOOL)" >&2; \
		exit 1; \
	fi

build_valgrind: check_compiler check_valgrind
	cmake -S . -B $(VALGRIND_BUILD_DIR) \
		-DCMAKE_C_COMPILER=$(SANITIZER_CC)
	cmake -S . -B $(VALGRIND_BUILD_DIR) \
		-DCMAKE_C_COMPILER=$(SANITIZER_CC) \
		-DCMAKE_BUILD_TYPE=RelWithDebInfo \
		-DBUILD_TESTING=ON \
		-DPG_STATUS_SANITIZER=none \
		-DPG_STATUS_ENABLE_CLANG_TIDY=OFF \
		-DMEMORYCHECK_TYPE=Valgrind \
		-DMEMORYCHECK_COMMAND=$(VALGRIND_TOOL) \
		-DMEMORYCHECK_COMMAND_OPTIONS="$(VALGRIND_OPTIONS)"
	cmake --build $(VALGRIND_BUILD_DIR) --parallel

test_valgrind: build_valgrind
	ctest --test-dir $(VALGRIND_BUILD_DIR) \
		-T memcheck \
		--output-on-failure \
		--no-tests=error

docker-compose-command := $(shell docker compose version >/dev/null 2>&1 && echo docker compose || echo docker-compose)
docker-compose := $(docker-compose-command) -f docker-compose.yml
test-docker-compose := $(docker-compose-command) -p test -f test/docker/docker-compose.yml

colima_start:
	colima start --arch aarch64 --vm-type=vz --vz-rosetta --cpu 6 --memory 8

build_push_amd_64:
	sudo docker build --platform linux/amd64 -f docker/alpine/Dockerfile_shared -t pg-status:${v} .
	sudo docker tag pg-status:${v} ${r}/pg-status:${v}
	sudo docker tag pg-status:${v} ${r}/pg-status:latest
	sudo docker push ${r}/pg-status:${v}
	sudo docker push ${r}/pg-status:latest

format-warn:
	cmake --build $(DEBUG_BUILD_DIR) --target format-warn

# CMake clears its cache when the compiler changes, so select it separately
# before applying the sanitizer profile options.
build_asan: check_compiler
	cmake -S . -B $(ASAN_BUILD_DIR) \
		-DCMAKE_C_COMPILER=$(SANITIZER_CC)
	cmake -S . -B $(ASAN_BUILD_DIR) \
		-DCMAKE_C_COMPILER=$(SANITIZER_CC) \
		-DCMAKE_BUILD_TYPE=Debug \
		-DBUILD_TESTING=ON \
		-DPG_STATUS_SANITIZER=address-undefined \
		-DPG_STATUS_ENABLE_CLANG_TIDY=ON
	cmake --build $(ASAN_BUILD_DIR) --parallel

test_asan: build_asan
	ctest --test-dir $(ASAN_BUILD_DIR) --output-on-failure \
		--no-tests=error

build_tsan: check_compiler
	cmake -S . -B $(TSAN_BUILD_DIR) \
		-DCMAKE_C_COMPILER=$(SANITIZER_CC)
	cmake -S . -B $(TSAN_BUILD_DIR) \
		-DCMAKE_C_COMPILER=$(SANITIZER_CC) \
		-DCMAKE_BUILD_TYPE=Debug \
		-DBUILD_TESTING=ON \
		-DPG_STATUS_SANITIZER=thread \
		-DPG_STATUS_ENABLE_CLANG_TIDY=ON
	cmake --build $(TSAN_BUILD_DIR) --parallel

test_tsan: build_tsan
	ctest --test-dir $(TSAN_BUILD_DIR) --output-on-failure \
		--no-tests=error

build_repeat: check_compiler
	cmake -S . -B $(REPEAT_BUILD_DIR) \
		-DCMAKE_C_COMPILER=$(SANITIZER_CC)
	cmake -S . -B $(REPEAT_BUILD_DIR) \
		-DCMAKE_C_COMPILER=$(SANITIZER_CC) \
		-DCMAKE_BUILD_TYPE=Release \
		-DBUILD_TESTING=ON \
		-DPG_STATUS_SANITIZER=none \
		-DPG_STATUS_ENABLE_CLANG_TIDY=ON
	cmake --build $(REPEAT_BUILD_DIR) --parallel

test_repeat: build_repeat
	ctest --test-dir $(REPEAT_BUILD_DIR) --output-on-failure \
		--no-tests=error \
		--repeat until-fail:$(REPEAT_COUNT)


test_repeat_asan: build_asan
	ctest --test-dir $(ASAN_BUILD_DIR) --output-on-failure \
		--no-tests=error \
		--repeat until-fail:$(REPEAT_COUNT)

test_repeat_tsan: build_tsan
	ctest --test-dir $(TSAN_BUILD_DIR) --output-on-failure \
		--no-tests=error \
		--repeat until-fail:$(REPEAT_COUNT)

pre-release:
	$(MAKE) scan-build
	$(MAKE) REPEAT_COUNT=$(REPEAT_COUNT) test_repeat_asan
	$(MAKE) REPEAT_COUNT=$(REPEAT_COUNT) test_repeat_tsan
	$(MAKE) REPEAT_COUNT=$(REPEAT_COUNT) test_repeat
ifeq ($(HOST_OS),Linux)
	$(MAKE) test_valgrind
else
	@printf 'Skipping Valgrind: supported by the Linux pre-release gate\n'
endif

pre-release-docker:
	docker build \
		--platform $(PRE_RELEASE_PLATFORM) \
		-f $(PRE_RELEASE_DOCKERFILE) \
		-t $(PRE_RELEASE_IMAGE) \
		.
	docker run --rm \
		--platform $(PRE_RELEASE_PLATFORM) \
		--security-opt seccomp=unconfined \
		--label $(PRE_RELEASE_CONTAINER_LABEL) \
		$(PRE_RELEASE_IMAGE) \
		make REPEAT_COUNT=$(REPEAT_COUNT) pre-release

release-builds: pre-release-docker
	$(MAKE) build_shared_alpine
	$(MAKE) build_static_alpine
	$(MAKE) build_shared_ubuntu
	$(MAKE) build_static_ubuntu
	$(MAKE) build_deb
	$(MAKE) build_shared_executable
	$(MAKE) build_static_executable
	$(MAKE) build_push r=krylosovaa v=2.1.1

benchmark_rps:
	./test/rps/run.sh
