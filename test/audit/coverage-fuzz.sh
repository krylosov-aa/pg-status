#!/usr/bin/env bash
set -Eeuo pipefail
cmake -S . -B /tmp/coverage -G Ninja -DCMAKE_C_COMPILER=clang \
  -DCMAKE_BUILD_TYPE=Debug -DPG_STATUS_ENABLE_COVERAGE=ON
cmake --build /tmp/coverage --parallel
mkdir -p /reports/coverage
LLVM_PROFILE_FILE=/reports/coverage/%p-%m.profraw \
  ctest --test-dir /tmp/coverage --output-on-failure --output-junit /reports/coverage/tests.xml
llvm-profdata merge -sparse /reports/coverage/*.profraw -o /reports/coverage/all.profdata
objects=(/tmp/coverage/src/pg-status)
while IFS= read -r binary; do objects+=(-object "$binary"); done < <(find /tmp/coverage/test -type f -executable)
llvm-cov export "${objects[@]}" -instr-profile=/reports/coverage/all.profdata \
  -ignore-filename-regex='(/test/|/usr/)' > /reports/coverage/coverage.json
llvm-cov report "${objects[@]}" -instr-profile=/reports/coverage/all.profdata \
  -ignore-filename-regex='(/test/|/usr/)' > /reports/coverage/summary.txt
# Source annotation is retained so uncovered production branches can be reviewed.
llvm-cov show "${objects[@]}" -instr-profile=/reports/coverage/all.profdata \
  -ignore-filename-regex='(/test/|/usr/)' -format=html \
  -output-dir=/reports/coverage/html
cmake -S . -B /tmp/fuzz -G Ninja -DCMAKE_C_COMPILER=clang \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=OFF \
  -DPG_STATUS_SANITIZER=address-undefined -DPG_STATUS_ENABLE_FUZZING=ON
cmake --build /tmp/fuzz --target fuzz_inputs fuzz_http --parallel
mkdir -p /reports/fuzz/corpus
printf 'FFFFFFFF/FFFFFFFF' > /reports/fuzz/corpus/lsn
printf '18446744073709551616' > /reports/fuzz/corpus/overflow
ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  /tmp/fuzz/test/fuzz/fuzz_inputs /reports/fuzz/corpus -max_total_time=30 \
  -timeout=5 -max_len=4096 -artifact_prefix=/reports/fuzz/
mkdir -p /reports/fuzz/http-corpus
printf '0/0' > /reports/fuzz/http-corpus/lsn
head -c 512 /dev/zero > /reports/fuzz/http-corpus/boundary
ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  /tmp/fuzz/test/fuzz/fuzz_http /reports/fuzz/http-corpus -max_total_time=30 \
  -timeout=5 -max_len=512 -artifact_prefix=/reports/fuzz/
