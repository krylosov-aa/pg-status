#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
state="${BENCH_STATE:-${root}/out/rps-state}"
mkdir -p "${state}"
if [[ -r "${state}/server.pid" ]] && kill -0 "$(cat "${state}/server.pid")" 2>/dev/null; then
  echo "Server already running" >&2; exit 2
fi
ulimit -n 8192
export pg_status__hosts="${pg_status__hosts:-127.0.0.1,127.0.0.2,127.0.0.3}"
export pg_status__http_listen_address=127.0.0.1
export pg_status__sleep_ms="${pg_status__sleep_ms:-1000}"
export pg_status__log_level=info
export pg_status__log_format=text
env | LC_ALL=C sort | awk '/^pg_status__/ && !/password/' >"${state}/server-env.txt"
nohup taskset -c "${SERVER_CPUS:-0}" "${root}/cmake-builds/release/src/pg-status" >"${state}/server.log" 2>&1 </dev/null &
echo "$!" >"${state}/server.pid"
for ((i=0;i<100;i++)); do
  if curl --fail --silent http://127.0.0.1:8000/ready >/dev/null; then
    curl --fail --silent http://127.0.0.1:8000/hosts | tee "${state}/initial-hosts.json"
    exit 0
  fi
  sleep 0.1
done
echo "Server did not become ready" >&2; exit 1
