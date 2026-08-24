#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

pg_status_url="${PG_STATUS_URL:-http://127.0.0.1:8000}"
target_rps="${TARGET_RPS:-5000}"
duration="${DURATION:-5m}"
warmup_duration="${WARMUP_DURATION:-30s}"
connections="${CONNECTIONS:-512}"
attack_workers="${ATTACK_WORKERS:-512}"
request_timeout="${REQUEST_TIMEOUT:-2s}"
server_http_workers="${SERVER_HTTP_WORKERS:-1}"
max_p99_ms="${MAX_P99_MS:-5}"
min_success_ratio="${MIN_SUCCESS_RATIO:-1.0}"
min_throughput_ratio="${MIN_THROUGHPUT_RATIO:-0.99}"
profile_file="${PROFILE_FILE:-${script_dir}/profile.txt}"
results_root="${RESULTS_DIR:-${script_dir}/results}"
keep_binary="${KEEP_BINARY:-0}"

fail() {
  printf 'rps benchmark: %s\n' "$*" >&2
  exit 2
}

for command in curl jq vegeta; do
  command -v "${command}" >/dev/null 2>&1 || fail "${command} is required"
done

[[ "${target_rps}" =~ ^[1-9][0-9]*$ ]] || fail "TARGET_RPS must be a positive integer"
[[ "${connections}" =~ ^[1-9][0-9]*$ ]] || fail "CONNECTIONS must be a positive integer"
[[ "${attack_workers}" =~ ^[1-9][0-9]*$ ]] || \
  fail "ATTACK_WORKERS must be a positive integer"
[[ "${server_http_workers}" =~ ^[1-9][0-9]*$ ]] || \
  fail "SERVER_HTTP_WORKERS must be a positive integer"
[[ -r "${profile_file}" ]] || fail "profile is not readable: ${profile_file}"

soft_nofile="$(ulimit -Sn)"
required_nofile=$((connections + 128))
if [[ "${soft_nofile}" =~ ^[0-9]+$ ]] &&
   ((soft_nofile < required_nofile)); then
  fail "open-file limit ${soft_nofile} is too low; run: ulimit -n 4096"
fi

if ! jq -en \
  --arg max_p99_ms "${max_p99_ms}" \
  --arg min_success_ratio "${min_success_ratio}" \
  --arg min_throughput_ratio "${min_throughput_ratio}" \
  '($max_p99_ms | tonumber) >= 0 and
   (($min_success_ratio | tonumber) >= 0 and
    ($min_success_ratio | tonumber) <= 1) and
   (($min_throughput_ratio | tonumber) >= 0 and
    ($min_throughput_ratio | tonumber) <= 1)' >/dev/null; then
  fail "MAX_P99_MS and ratio values must be valid non-negative numbers; ratios must not exceed 1"
fi

pg_status_url="${pg_status_url%/}"
timestamp="$(date -u +%Y%m%dT%H%M%SZ)"
run_dir="${results_root}/${timestamp}-rps-${target_rps}-$$"
mkdir -p "${run_dir}"

vegeta_targets="$(mktemp "${TMPDIR:-/tmp}/pg-status-rps-targets.XXXXXX")"
trap 'rm -f "${vegeta_targets}"' EXIT

target_count=0
line_number=0
while read -r weight path extra || [[ -n "${weight:-}${path:-}${extra:-}" ]]; do
  line_number=$((line_number + 1))
  case "${weight:-}" in
    ''|'#'*)
      continue
      ;;
  esac

  [[ "${weight}" =~ ^[1-9][0-9]*$ ]] || \
    fail "invalid weight at ${profile_file}:${line_number}"
  [[ "${path:-}" == /* ]] || \
    fail "path must start with '/' at ${profile_file}:${line_number}"

  for ((i = 0; i < weight; i++)); do
    printf 'GET %s%s\n\n' "${pg_status_url}" "${path}" >>"${vegeta_targets}"
    target_count=$((target_count + 1))
  done
done <"${profile_file}"

((target_count > 0)) || fail "profile contains no targets"

curl --fail --silent --show-error --max-time 2 \
  "${pg_status_url}/version" >/dev/null || \
  fail "pg-status is not ready at ${pg_status_url}"

cp "${profile_file}" "${run_dir}/profile.txt"
{
  printf 'timestamp=%s\n' "${timestamp}"
  printf 'pg_status_url=%s\n' "${pg_status_url}"
  printf 'target_rps=%s\n' "${target_rps}"
  printf 'duration=%s\n' "${duration}"
  printf 'warmup_duration=%s\n' "${warmup_duration}"
  printf 'connections=%s\n' "${connections}"
  printf 'request_timeout=%s\n' "${request_timeout}"
  printf 'max_p99_ms=%s\n' "${max_p99_ms}"
  printf 'min_success_ratio=%s\n' "${min_success_ratio}"
  printf 'min_throughput_ratio=%s\n' "${min_throughput_ratio}"
  printf 'profile_targets=%s\n' "${target_count}"
  printf 'system='
  uname -a
} >"${run_dir}/metadata.txt"

attack_options=(
  "-targets=${vegeta_targets}"
  "-rate=${target_rps}/s"
  "-connections=${connections}"
  "-max-connections=${connections}"
  "-workers=${attack_workers}"
  "-max-workers=${attack_workers}"
  "-timeout=${request_timeout}"
  "-max-body=0"
)

printf 'Warming up %s at %s RPS over %s targets...\n' \
  "${pg_status_url}" "${target_rps}" "${target_count}"
vegeta attack \
  "${attack_options[@]}" \
  "-duration=${warmup_duration}" \
  >/dev/null

attack_file="${run_dir}/attack.bin"
text_report="${run_dir}/report.txt"
json_report="${run_dir}/report.json"

printf 'Measuring for %s...\n' "${duration}"
vegeta attack \
  "${attack_options[@]}" \
  "-duration=${duration}" \
  >"${attack_file}"

vegeta report "${attack_file}" | tee "${text_report}"
vegeta report -type=json "${attack_file}" >"${json_report}"

printf '\nRequired: success >= %s, throughput >= %.2f RPS, p99 <= %s ms\n' \
  "${min_success_ratio}" \
  "$(jq -n "${target_rps} * ${min_throughput_ratio}")" \
  "${max_p99_ms}"

if jq -e \
  --argjson target_rps "${target_rps}" \
  --argjson max_p99_ms "${max_p99_ms}" \
  --argjson min_success_ratio "${min_success_ratio}" \
  --argjson min_throughput_ratio "${min_throughput_ratio}" \
  '(.success >= $min_success_ratio) and
   (.throughput >= ($target_rps * $min_throughput_ratio)) and
   (.latencies["99th"] <= ($max_p99_ms * 1000000))' \
  "${json_report}" >/dev/null; then
  result=0
  printf 'PASS: %s RPS is sustainable under the configured SLO.\n' "${target_rps}"
else
  result=1
  printf 'FAIL: %s RPS does not satisfy the configured SLO.\n' "${target_rps}" >&2
fi

if [[ "${keep_binary}" != 1 ]]; then
  rm -f "${attack_file}"
fi

printf 'Reports: %s\n' "${run_dir}"
exit "${result}"
