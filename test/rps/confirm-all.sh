#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${root}"
ulimit -n 8192
results="test/rps/results/campaign"
taskset -c 1 python3 -u test/rps/suite.py confirm >"${results}/confirmation.log" 2>&1
taskset -c 1 python3 -u test/rps/extras.py >"${results}/extras.log" 2>&1
taskset -c 1 python3 test/rps/summarize.py "${results}" \
  --output "${results}/summary.json" >"${results}/summary.md"
