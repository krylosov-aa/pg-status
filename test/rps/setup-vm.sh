#!/usr/bin/env bash
# Dedicated Ubuntu 24.04 benchmark VM only. Run as the SSH user, not root.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
state="${BENCH_STATE:-${root}/out/rps-state}"
pg_bin="${PG_BIN:-/usr/lib/postgresql/16/bin}"
mkdir -p "${state}"
[[ ! -e "${state}/primary" ]] || { echo "Already initialized: ${state}" >&2; exit 2; }
sudo -n systemctl stop postgresql
"${pg_bin}/initdb" -D "${state}/primary" -U postgres -A trust --no-instructions >"${state}/initdb.log"
cat >>"${state}/primary/postgresql.conf" <<EOF
listen_addresses = '127.0.0.1'
port = 5432
unix_socket_directories = '${state}'
shared_buffers = '64MB'
max_connections = 30
wal_level = replica
max_wal_senders = 10
wal_keep_size = '256MB'
max_wal_size = '1GB'
checkpoint_timeout = '5min'
fsync = on
synchronous_commit = on
full_page_writes = on
ssl = off
EOF
taskset -c 1 "${pg_bin}/pg_ctl" -D "${state}/primary" -l "${state}/primary.log" -w start
for i in 2 3; do
  "${pg_bin}/pg_basebackup" -h 127.0.0.1 -U postgres -D "${state}/replica${i}" -Fp -Xs -R -c fast >"${state}/basebackup${i}.log" 2>&1
  cat >>"${state}/replica${i}/postgresql.conf" <<EOF
listen_addresses = '127.0.0.${i}'
unix_socket_directories = ''
hot_standby = on
EOF
  taskset -c 1 "${pg_bin}/pg_ctl" -D "${state}/replica${i}" -l "${state}/replica${i}.log" -w start
done
psql -h 127.0.0.1 -U postgres -d postgres -v ON_ERROR_STOP=1 <<'SQL'
CREATE TABLE bench_wal (id integer PRIMARY KEY, version bigint NOT NULL, payload text NOT NULL);
INSERT INTO bench_wal SELECT n, 0, repeat('x', 1024) FROM generate_series(1,100) n;
SELECT pg_switch_wal();
SQL
printf '%s\n' "${state}" >"${root}/out/rps-state-path"
echo "Topology ready at ${state}. Start pg-status with start-server.sh."
