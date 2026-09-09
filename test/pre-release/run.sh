#!/usr/bin/env bash
set -Eeuo pipefail
# Reports survive --rm even when a gate fails.
collect_reports() {
  local directory
  for directory in cmake-builds/*; do
    if [[ -d "$directory/Testing" ]]; then
      mkdir -p "/reports/$(basename "$directory")"
      cp -R "$directory/Testing" "/reports/$(basename "$directory")/"
    fi
  done
}
trap collect_reports EXIT
shellcheck /usr/local/bin/run-gates test/configure.sh test/audit/*.sh
make SCAN_REPORT_DIR=/reports/scan "$@"
