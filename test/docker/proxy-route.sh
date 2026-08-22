#!/usr/bin/env bash

set -Eeuo pipefail

if [[ "$#" -ne 2 ]]; then
  printf 'usage: %s <proxy> <primary|replica_1|replica_2|none>\n' "$0" >&2
  exit 2
fi

readonly proxy="$1"
readonly selected_backend="$2"
readonly script_directory="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
readonly compose_file="$script_directory/docker-compose.yml"

case "$proxy" in
  pg-proxy-1 | pg-proxy-2 | pg-proxy-3) ;;
  *)
    printf 'unknown proxy: %s\n' "$proxy" >&2
    exit 2
    ;;
esac

case "$selected_backend" in
  primary | replica_1 | replica_2 | none) ;;
  *)
    printf 'unknown backend: %s\n' "$selected_backend" >&2
    exit 2
    ;;
esac

if docker compose version >/dev/null 2>&1; then
  compose=(docker compose)
else
  compose=(docker-compose)
fi

haproxy_command() {
  printf '%s\n' "$1" | "${compose[@]}" \
    --project-name test \
    --file "$compose_file" \
    --profile pg-status \
    exec -T "$proxy" socat stdio /run/haproxy/admin.sock >/dev/null
}

for backend in primary replica_1 replica_2; do
  if [[ "$backend" == "$selected_backend" ]]; then
    haproxy_command "enable server pg_backends/$backend"
  else
    haproxy_command "disable server pg_backends/$backend"
    haproxy_command "shutdown sessions server pg_backends/$backend"
  fi
done
