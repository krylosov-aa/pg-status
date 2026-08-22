#!/usr/bin/env bash

set -Eeuo pipefail

export BUILDKIT_PROGRESS="${BUILDKIT_PROGRESS:-plain}"

readonly AUDIT_CONTAINER_LABEL_KEY="com.pg-status.audit.run"
readonly AUDIT_ROLE_LABEL="com.pg-status.role=audit"

script_directory="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
repository_root="$(cd "$script_directory/.." && pwd -P)"
cd "$repository_root"

audit_platform="${AUDIT_PLATFORM:-linux/amd64}"
audit_repeat_count="${AUDIT_REPEAT_COUNT:-100}"
audit_emulated_repeat_count="${AUDIT_EMULATED_REPEAT_COUNT:-1}"
audit_pull="${AUDIT_PULL:-1}"
audit_no_cache="${AUDIT_NO_CACHE:-1}"
audit_artifact_root="${AUDIT_ARTIFACT_ROOT:-out}"
audit_run_id="$(date -u +%Y%m%dT%H%M%SZ)-$$"
audit_run_label="${AUDIT_CONTAINER_LABEL_KEY}=${audit_run_id}"
audit_pre_release_image="pg-status-pre-release:audit-${audit_run_id}"
audit_native_image="pg-status-pre-release:native-${audit_run_id}"
audit_artifact_directory="${AUDIT_ARTIFACT_DIR:-${audit_artifact_root}/audit-${audit_run_id}}"

if [[ "$audit_artifact_directory" = /* ]]; then
  audit_artifact_path="$audit_artifact_directory"
else
  audit_artifact_path="$repository_root/$audit_artifact_directory"
fi

audit_runtime_container=""
audit_temporary_directory=""
audit_baseline_file=""
audit_cleanup_baseline_file=""
audit_has_git_metadata=0

print_stage() {
  printf '\n==> %s\n' "$1"
}

fail() {
  printf 'full-audit: %s\n' "$1" >&2
  exit 1
}

require_command() {
  command -v "$1" >/dev/null 2>&1 || fail "required command not found: $1"
}

validate_boolean() {
  case "$2" in
    0 | 1) ;;
    *) fail "$1 must be 0 or 1, got: $2" ;;
  esac
}

record_running_containers() {
  local container_id

  docker ps --quiet | while IFS= read -r container_id; do
    if [[ -n "$container_id" ]]; then
      docker inspect \
        --format '{{.Id}} {{.Name}} {{.State.StartedAt}}' \
        "$container_id"
    fi
  done | LC_ALL=C sort
}

write_source_manifest() {
  python3 - "$repository_root" "$audit_artifact_path" <<'PY'
import hashlib
import os
import sys

root = sys.argv[1]
artifact_path = os.path.realpath(sys.argv[2])
excluded = {
    '.git',
    '.idea',
    '.mypy_cache',
    '.pytest_cache',
    '.ruff_cache',
    '.uv-cache',
    '.venv',
    'cmake-builds',
    'scan_reports',
    'out',
}

for base, directories, files in os.walk(root):
    directories[:] = sorted(
        directory
        for directory in directories
        if directory not in excluded
        and os.path.realpath(os.path.join(base, directory)) != artifact_path
    )
    for filename in sorted(files):
        path = os.path.join(base, filename)
        relative_path = os.path.relpath(path, root)
        digest = hashlib.sha256()
        with open(path, 'rb') as source_file:
            for chunk in iter(lambda: source_file.read(1024 * 1024), b''):
                digest.update(chunk)
        print(digest.hexdigest(), relative_path)
PY
}

stop_runtime_container() {
  if [[ -n "$audit_runtime_container" ]]; then
    docker stop --time 10 "$audit_runtime_container" >/dev/null 2>&1 || true
    audit_runtime_container=""
  fi
}

cleanup() {
  local exit_status="$1"
  local container_id

  trap - EXIT INT TERM
  stop_runtime_container

  if command -v docker >/dev/null 2>&1; then
    docker ps --quiet --filter "label=$audit_run_label" |
      while IFS= read -r container_id; do
        if [[ -n "$container_id" ]]; then
          docker stop --time 10 "$container_id" >/dev/null 2>&1 || true
        fi
      done

    if [[ -n "$audit_baseline_file" && -f "$audit_baseline_file" ]]; then
      record_running_containers >"$audit_cleanup_baseline_file" || true
      if ! cmp -s "$audit_baseline_file" "$audit_cleanup_baseline_file"; then
        printf '%s\n' \
          'full-audit: running container identity/start time changed:' >&2
        diff -u "$audit_baseline_file" "$audit_cleanup_baseline_file" >&2 || true
        if [[ "$exit_status" -eq 0 ]]; then
          exit_status=1
        fi
      fi
    fi
  fi

  if [[ -n "$audit_temporary_directory" && -d "$audit_temporary_directory" ]]; then
    find "$audit_temporary_directory" -type f -delete 2>/dev/null || true
    rmdir "$audit_temporary_directory" 2>/dev/null || true
  fi

  exit "$exit_status"
}

smoke_runtime_image() {
  local image="$1"
  local port
  local version

  printf 'Smoke testing %s\n' "$image"
  audit_runtime_container="$(docker run --rm --detach \
    --platform "$audit_platform" \
    --label "$AUDIT_ROLE_LABEL" \
    --label "$audit_run_label" \
    --publish 127.0.0.1::8000 \
    --env pg_status__hosts=127.0.0.1 \
    --env pg_status__pg_port=1 \
    --env pg_status__connect_timeout=1 \
    --env pg_status__sleep_ms=1000 \
    "$image")"

  port="$(docker port "$audit_runtime_container" 8000/tcp |
    sed -n '1s/.*://p')"
  [[ -n "$port" ]] || fail "Docker did not publish the HTTP port for $image"

  if ! version="$(curl --fail --silent --show-error \
    --retry 20 --retry-all-errors --retry-connrefused --retry-delay 1 \
    "http://127.0.0.1:${port}/version")"; then
    docker logs "$audit_runtime_container" >&2 || true
    fail "HTTP smoke test failed for $image"
  fi
  [[ "$version" == "$project_version" ]] ||
    fail "unexpected version from $image: $version"

  docker stop --time 10 "$audit_runtime_container" >/dev/null
  audit_runtime_container=""
}

# Stage 0: Validate host prerequisites, requested settings, and Docker
# capabilities before creating any audit resources.
for required_command in bash curl diff docker find make python3 sed uv; do
  require_command "$required_command"
done

[[ "$audit_platform" == "linux/amd64" ]] ||
  fail "full release audit currently requires AUDIT_PLATFORM=linux/amd64"
[[ "$audit_repeat_count" =~ ^[1-9][0-9]*$ ]] ||
  fail "AUDIT_REPEAT_COUNT must be a positive integer"
[[ "$audit_emulated_repeat_count" =~ ^[1-9][0-9]*$ ]] ||
  fail "AUDIT_EMULATED_REPEAT_COUNT must be a positive integer"
validate_boolean AUDIT_PULL "$audit_pull"
validate_boolean AUDIT_NO_CACHE "$audit_no_cache"

project_version="$(sed -nE \
  's/^project\(pg-status VERSION ([0-9]+\.[0-9]+\.[0-9]+) LANGUAGES C\)$/\1/p' \
  CMakeLists.txt)"
[[ -n "$project_version" ]] || fail "cannot read project version from CMakeLists.txt"

if docker compose version >/dev/null 2>&1; then
  audit_compose=(docker compose)
elif command -v docker-compose >/dev/null 2>&1; then
  audit_compose=(docker-compose)
else
  fail "Docker Compose plugin or docker-compose executable is required"
fi

docker_server_architecture="$(docker info --format '{{.Architecture}}')"
case "$docker_server_architecture" in
  amd64 | x86_64)
    docker_server_platform="linux/amd64"
    ;;
  arm64 | aarch64)
    docker_server_platform="linux/arm64"
    ;;
  *)
    docker_server_platform="linux/$docker_server_architecture"
    ;;
esac

if docker buildx version >/dev/null 2>&1; then
  docker_builder=(docker buildx build --load)
elif command -v docker-buildx >/dev/null 2>&1 &&
  docker-buildx version >/dev/null 2>&1; then
  docker_builder=(docker-buildx build --load)
elif [[ "$docker_server_platform" == "$audit_platform" ]]; then
  docker_builder=(docker build)
else
  fail "Docker buildx is required to build $audit_platform on $docker_server_platform"
fi

audit_temporary_directory="$(mktemp -d \
  "${TMPDIR:-/tmp}/pg-status-full-audit.XXXXXX")"
audit_baseline_file="$audit_temporary_directory/containers-before.txt"
audit_cleanup_baseline_file="$audit_temporary_directory/containers-after-cleanup.txt"
source_before_file="$audit_temporary_directory/source-before.sha256"
source_after_file="$audit_temporary_directory/source-after.sha256"
trap 'cleanup "$?"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

docker_build=("${docker_builder[@]}" --platform "$audit_platform")
native_docker_build=("${docker_builder[@]}" --platform "$docker_server_platform")
pre_release_repeat_count="$audit_repeat_count"
if [[ "$docker_server_platform" != "$audit_platform" ]]; then
  pre_release_repeat_count="$audit_emulated_repeat_count"
fi
release_docker_build_flags=()
if [[ "$audit_pull" == 1 ]]; then
  docker_build+=(--pull)
  native_docker_build+=(--pull)
  release_docker_build_flags+=(--pull)
fi
if [[ "$audit_no_cache" == 1 ]]; then
  docker_build+=(--no-cache)
  native_docker_build+=(--no-cache)
  release_docker_build_flags+=(--no-cache)
fi

# Stage 1: Capture immutable baselines for the source tree and running
# containers. The final stage compares the current state with these snapshots.
print_stage "Record source and Docker state"
if command -v git >/dev/null 2>&1 &&
  git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  audit_has_git_metadata=1
  git status --short
  git diff HEAD --check
else
  printf '%s\n' \
    'Git metadata unavailable; source integrity will use the SHA-256 manifest.'
fi
write_source_manifest >"$source_before_file"
docker version
"${docker_builder[@]}" --help >/dev/null
"${audit_compose[@]}" version

if [[ -n "$(docker ps --quiet --filter "label=$audit_run_label")" ]]; then
  fail "unexpected running container already has label $audit_run_label"
fi
record_running_containers >"$audit_baseline_file"
docker ps --format '{{.ID}} {{.Names}} {{.Status}}'

# Stage 2: Run the complete analyzer, formatting, sanitizer, Release, and
# Valgrind pre-release gate against the requested Linux amd64 platform.
print_stage "Fresh Linux amd64 pre-release gate"
"${docker_build[@]}" \
  --label "$audit_run_label" \
  -f test/pre-release/Dockerfile \
  -t "$audit_pre_release_image" \
  .
docker run --rm \
  --platform "$audit_platform" \
  --security-opt seccomp=unconfined \
  --label "$AUDIT_ROLE_LABEL" \
  --label "$audit_run_label" \
  "$audit_pre_release_image" \
  make \
    REPEAT_COUNT="$pre_release_repeat_count" \
    PRE_RELEASE_TSAN="$([[ "$docker_server_platform" == "$audit_platform" ]] && printf 1 || printf 0)" \
    pre-release

if [[ "$docker_server_platform" != "$audit_platform" ]]; then
  # Stage 2a: Large repeated suites and TSan cannot run reliably through
  # architecture emulation. Keep the target-platform pass above, then perform
  # the stability repetitions with the native architecture.
  print_stage "Native $docker_server_platform repeated sanitizer and Release gates"
  "${native_docker_build[@]}" \
    --label "$audit_run_label" \
    -f test/pre-release/Dockerfile \
    -t "$audit_native_image" \
    .
  docker run --rm \
    --platform "$docker_server_platform" \
    --security-opt seccomp=unconfined \
    --label "$AUDIT_ROLE_LABEL" \
    --label "$audit_run_label" \
    "$audit_native_image" \
    make REPEAT_COUNT="$audit_repeat_count" \
      test_repeat_asan test_repeat_tsan test_repeat
fi

# Stage 3: Exercise a second GCC-based CMake configure/build/test/install path
# with author warnings promoted to errors.
print_stage "Independent strict CMake audit"
docker run --rm \
  --platform "$audit_platform" \
  --label "$AUDIT_ROLE_LABEL" \
  --label "$audit_run_label" \
  "$audit_pre_release_image" \
  sh -euxc '
    cmake --list-presets
    cmake --preset release
    cmake --build --preset release --parallel
    ctest --preset release

    cmake -S . -B /tmp/pg-status-cmake-author-audit -G Ninja \
      -Wdev -Werror=dev --warn-uninitialized \
      -DCMAKE_C_COMPILER=gcc \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_TESTING=ON \
      -DPG_STATUS_SANITIZER=none \
      -DPG_STATUS_ENABLE_CLANG_TIDY=OFF
    cmake --build /tmp/pg-status-cmake-author-audit --parallel
    ctest --test-dir /tmp/pg-status-cmake-author-audit \
      --output-on-failure --no-tests=error
    cmake --build /tmp/pg-status-cmake-author-audit \
      --target format-check
    cmake --install /tmp/pg-status-cmake-author-audit \
      --prefix /tmp/pg-status-install
    test -x /tmp/pg-status-install/bin/pg-status
  '

# Stage 4: Validate Compose models, shell syntax, and important Make entry
# points without starting application or infrastructure Compose projects.
print_stage "Auxiliary configuration and script validation"
"${audit_compose[@]}" \
  --env-file .env_example -f docker-compose.yml config --quiet
"${audit_compose[@]}" \
  -f test/docker/docker-compose.yml config --quiet
while IFS= read -r shell_script; do
  bash -n "$shell_script"
done < <(find test -type f -name '*.sh' -print)
make --dry-run pre-release-docker >/dev/null
make --dry-run release-builds >/dev/null
make --dry-run test_e2e_all >/dev/null

# Stage 4a: Exercise the production monitor against real PostgreSQL under
# every supported runtime instrumentation profile. These remain separate from
# CTest so the normal development loop does not require Docker.
print_stage "PostgreSQL e2e matrix (Release, ASan/UBSan, TSan, Valgrind)"
make test_e2e_all

# Stage 5: Build every supported production runtime image for Linux amd64.
print_stage "Production runtime images"
alpine_shared_image="pg-status-audit:alpine-shared-${audit_run_id}"
alpine_static_image="pg-status-audit:alpine-static-${audit_run_id}"
ubuntu_shared_image="pg-status-audit:ubuntu-shared-${audit_run_id}"
ubuntu_static_image="pg-status-audit:ubuntu-static-${audit_run_id}"

"${docker_build[@]}" --label "$audit_run_label" \
  -f docker/alpine/Dockerfile_shared -t "$alpine_shared_image" .
"${docker_build[@]}" --label "$audit_run_label" \
  -f docker/alpine/Dockerfile_static -t "$alpine_static_image" .
"${docker_build[@]}" --label "$audit_run_label" \
  -f docker/ubuntu/Dockerfile_shared -t "$ubuntu_shared_image" .
"${docker_build[@]}" --label "$audit_run_label" \
  -f docker/ubuntu/Dockerfile_static -t "$ubuntu_static_image" .

# Stage 6: Produce the DEB package and shared/static release archives.
print_stage "Release packages"
make \
  RELEASE_PLATFORM="$audit_platform" \
  RELEASE_ARCH=amd64 \
  RELEASE_DOCKER_BUILD_FLAGS="${release_docker_build_flags[*]-}" \
  ARTIFACT_DIR="$audit_artifact_path" \
  build_deb build_shared_executable build_static_executable

# Stage 7: Start each production image on a dynamically allocated host port
# and verify its HTTP version endpoint.
print_stage "Runtime image smoke tests"
smoke_runtime_image "$alpine_shared_image"
smoke_runtime_image "$alpine_static_image"
smoke_runtime_image "$ubuntu_shared_image"
smoke_runtime_image "$ubuntu_static_image"

# Stage 8: Inspect package contents, licenses, and dynamic/static linkage, then
# install and execute every distributable binary.
print_stage "Packaged artifact inspection and smoke tests"
docker run --rm \
  --platform "$audit_platform" \
  --label "$AUDIT_ROLE_LABEL" \
  --label "$audit_run_label" \
  --env "PG_STATUS_AUDIT_VERSION=$project_version" \
  --volume "$audit_artifact_path:/artifacts:ro" \
  ubuntu:24.04 \
  sh -euxc '
    shared_archive="/artifacts/shared/pg-status_${PG_STATUS_AUDIT_VERSION}_linux_amd64_shared.tar.gz"
    static_archive="/artifacts/static/pg-status_${PG_STATUS_AUDIT_VERSION}_linux_amd64_static.tar.gz"
    deb_package="/artifacts/deb/pg-status_${PG_STATUS_AUDIT_VERSION}_amd64.deb"

    test -f "$shared_archive"
    test -f "$static_archive"
    test -f "$deb_package"

    apt-get update
    apt-get install -y --no-install-recommends curl file
    mkdir -p /tmp/shared /tmp/static
    tar -xzf "$shared_archive" -C /tmp/shared
    tar -xzf "$static_archive" -C /tmp/static

    test -x /tmp/shared/pg-status
    test -f /tmp/shared/LICENSE
    test -f /tmp/shared/THIRD-PARTY-NOTICES
    test -x /tmp/static/pg-status
    test -f /tmp/static/licenses/LICENSE-cJSON
    test -f /tmp/static/licenses/LICENSE-OpenSSL
    test -f /tmp/static/licenses/LICENSE-PostgreSQL
    test -f /tmp/static/licenses/LICENSE-libevent
    test -f /tmp/static/licenses/LICENSE-zlib

    dpkg-deb --info "$deb_package"
    dpkg-deb --contents "$deb_package"
    apt-get install -y "$deb_package"

    file /tmp/shared/pg-status /tmp/static/pg-status /usr/bin/pg-status
    ldd /tmp/shared/pg-status
    if ldd /tmp/shared/pg-status | grep -q "not found"; then
      echo "shared artifact has unresolved libraries" >&2
      exit 1
    fi
    if ldd /tmp/static/pg-status; then
      echo "static artifact is dynamically linked" >&2
      exit 1
    fi
    ldd /usr/bin/pg-status
    if ldd /usr/bin/pg-status | grep -q "not found"; then
      echo "installed DEB has unresolved libraries" >&2
      exit 1
    fi

    smoke_binary() {
      audit_binary="$1"
      audit_http_port="$2"
      env \
        pg_status__hosts=127.0.0.1 \
        pg_status__pg_port=1 \
        pg_status__connect_timeout=1 \
        pg_status__sleep_ms=1000 \
        pg_status__http_listen_address=127.0.0.1 \
        pg_status__http_port="$audit_http_port" \
        "$audit_binary" &
      audit_pid=$!
      trap "kill -TERM $audit_pid 2>/dev/null || true" EXIT

      audit_version="$(curl --fail --silent --show-error \
        --retry 20 --retry-all-errors --retry-connrefused --retry-delay 1 \
        "http://127.0.0.1:${audit_http_port}/version")"
      test "$audit_version" = "$PG_STATUS_AUDIT_VERSION"
      kill -TERM "$audit_pid"
      wait "$audit_pid"
      trap - EXIT
    }

    smoke_binary /tmp/shared/pg-status 18001
    smoke_binary /tmp/static/pg-status 18002
    smoke_binary /usr/bin/pg-status 18003
  '

# Stage 9: Prove that the audit neither modified source inputs nor restarted,
# stopped, or replaced containers that were already running on the host.
print_stage "Final source and Docker safety checks"
write_source_manifest >"$source_after_file"
diff -u "$source_before_file" "$source_after_file"
if [[ "$audit_has_git_metadata" == 1 ]]; then
  git diff HEAD --check
fi

if [[ -n "$(docker ps --quiet --filter "label=$audit_run_label")" ]]; then
  fail "an audit container is still running"
fi

record_running_containers >"$audit_cleanup_baseline_file"
diff -u "$audit_baseline_file" "$audit_cleanup_baseline_file"
docker ps --format '{{.ID}} {{.Names}} {{.Status}}'

printf '\nFull audit passed.\n'
printf 'Run ID: %s\n' "$audit_run_id"
printf 'Platform: %s\n' "$audit_platform"
printf 'Artifacts: %s\n' "$audit_artifact_path"
printf 'Repeat count: %s\n' "$audit_repeat_count"
if [[ "$docker_server_platform" != "$audit_platform" ]]; then
  printf 'Target-platform repeat count under emulation: %s\n' \
    "$audit_emulated_repeat_count"
fi
