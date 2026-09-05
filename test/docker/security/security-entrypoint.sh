#!/usr/bin/env bash
# Creates ordinary password and certificate-authenticated monitor roles.

set -Eeuo pipefail

psql \
  --set ON_ERROR_STOP=1 \
  --username "$POSTGRES_USER" \
  --dbname "$POSTGRES_DB" <<'EOSQL'
CREATE ROLE password_monitor LOGIN
  PASSWORD $$space 'quote' \backslash password$$;
CREATE ROLE mtls_monitor LOGIN;
EOSQL

cat >"$PGDATA/pg_hba.conf" <<'EOF'
local all all trust
hostssl all mtls_monitor 0.0.0.0/0 cert
hostssl all mtls_monitor ::/0 cert
hostssl all password_monitor 0.0.0.0/0 scram-sha-256
hostssl all password_monitor ::/0 scram-sha-256
hostnossl all password_monitor 0.0.0.0/0 scram-sha-256
hostnossl all password_monitor ::/0 scram-sha-256
EOF

cat >>"$PGDATA/postgresql.conf" <<'EOF'
listen_addresses = '*'
ssl = on
ssl_ca_file = '/tls/ca.crt'
ssl_cert_file = '/tls/server.crt'
ssl_key_file = '/tls/server.key'
EOF
