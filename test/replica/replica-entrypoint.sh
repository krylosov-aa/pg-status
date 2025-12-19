#!/bin/bash
set -e

DATA_DIR="${PGDATA:-/var/lib/postgresql/data}"

if [ "$REPLICATION_ROLE" = "replica" ]; then
  echo "Waiting for master..."
  until pg_isready -h "$MASTER_HOST" -p 5432 -U "$MASTER_USER"; do sleep 2; done
  echo "Master is ready, starting base backup..."

  rm -rf "$DATA_DIR"/*
  export PGPASSWORD="$MASTER_PASSWORD"

  pg_basebackup -h "$MASTER_HOST" \
      -D "$DATA_DIR" \
      -U "$MASTER_USER" \
      -Fp -Xs -P -R

  chown -R postgres:postgres "$DATA_DIR"
  chmod 700 "$DATA_DIR"
fi

exec docker-entrypoint.sh postgres
