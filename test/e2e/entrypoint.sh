#!/usr/bin/env bash

set -Eeuo pipefail

if [[ "${PG_STATUS_E2E_MODE:-release}" == "valgrind" ]]; then
  exec valgrind \
    --tool=memcheck \
    --leak-check=full \
    --show-leak-kinds=all \
    --errors-for-leak-kinds=definite,indirect \
    --track-origins=yes \
    --num-callers=50 \
    --error-exitcode=99 \
    /app/pg-status
fi

exec /app/pg-status
