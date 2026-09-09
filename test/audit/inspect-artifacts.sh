#!/usr/bin/env bash
set -Eeuo pipefail
export LC_ALL=C
version="$1"
mkdir -p /tmp/inspect/shared /tmp/inspect/static /tmp/inspect/deb
for kind in shared static; do
  tar -xzf "/artifacts/$kind/pg-status_${version}_linux_amd64_${kind}.tar.gz" -C "/tmp/inspect/$kind"
done
dpkg-deb -x "/artifacts/deb/pg-status_${version}_amd64.deb" /tmp/inspect/deb
for binary in /tmp/inspect/shared/pg-status /tmp/inspect/static/pg-status /tmp/inspect/deb/usr/bin/pg-status; do
  readelf --file-header "$binary" | tee /tmp/header
  grep -q 'Machine:.*Advanced Micro Devices X86-64' /tmp/header
  test -x "$binary"
done
readelf --dynamic /tmp/inspect/static/pg-status > /tmp/static-dynamic
if grep -q '(NEEDED)' /tmp/static-dynamic; then
  echo 'static artifact has dynamic dependencies' >&2
  exit 1
fi
readelf --program-headers /tmp/inspect/static/pg-status > /tmp/static-program
if grep -q INTERP /tmp/static-program; then
  echo 'static artifact requires an ELF interpreter' >&2
  exit 1
fi
for binary in /tmp/inspect/shared/pg-status /tmp/inspect/deb/usr/bin/pg-status; do
  readelf --dynamic "$binary" > /tmp/shared-dynamic
  grep -q '(NEEDED)' /tmp/shared-dynamic
done
