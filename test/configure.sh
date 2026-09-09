#!/usr/bin/env bash
set -Eeuo pipefail

if [[ "$#" -lt 2 ]]; then
  echo 'usage: configure.sh <compiler> <build-directory> [cmake-options...]' >&2
  exit 2
fi
compiler="$(command -v "$1")" || {
  printf 'C compiler not found: %s\n' "$1" >&2
  exit 1
}
build_directory="$2"
shift 2

# Changing compilers makes CMake discard profile options from that invocation.
# Perform the extra configure only when an existing cache uses another compiler.
cache="$build_directory/CMakeCache.txt"
if [[ -f "$cache" ]]; then
  cached_compiler="$(sed -n 's/^CMAKE_C_COMPILER:[^=]*=//p' "$cache")"
  if [[ "$cached_compiler" != "$compiler" ]]; then
    cmake -S . -B "$build_directory" "-DCMAKE_C_COMPILER=$compiler"
  fi
fi
# Do not carry child-process instrumentation into a different test profile.
cmake -S . -B "$build_directory" "-DCMAKE_C_COMPILER=$compiler" \
  -DPG_STATUS_TEST_VALGRIND= -DPG_STATUS_TEST_VALGRIND_OPTIONS= "$@"
