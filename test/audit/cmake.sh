#!/usr/bin/env bash
set -Eeuo pipefail
cmake --preset release -Wdev -Werror=dev --warn-uninitialized -DCMAKE_C_COMPILER=gcc
cmake --build --preset release --parallel
ctest --preset release --output-junit /reports/gcc.xml
cmake -S . -B /tmp/cmake-install -G Ninja -Wdev -Werror=dev --warn-uninitialized \
  -DCMAKE_C_COMPILER=gcc -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build /tmp/cmake-install --parallel
cmake --install /tmp/cmake-install --prefix /tmp/install
# Exercise installed main(), including clean SIGTERM, within a hard deadline.
# Expansion is deliberately performed by the child shell.
# shellcheck disable=SC2016
timeout --kill-after=5 20 bash -c '
  env pg_status__hosts=127.0.0.1 pg_status__pg_port=1 pg_status__http_port=18000 /tmp/install/bin/pg-status &
  pid=$!
  trap "kill -TERM $pid 2>/dev/null || true" EXIT
  curl --fail --silent --show-error --max-time 2 --retry 10 --retry-all-errors --retry-delay 1 http://127.0.0.1:18000/live
  kill -TERM "$pid"
  wait "$pid"
'
