#!/usr/bin/env bash

set -Eeuo pipefail

script_directory="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"

"$script_directory/proxy-route.sh" pg-proxy-1 primary
"$script_directory/proxy-route.sh" pg-proxy-2 replica_1
"$script_directory/proxy-route.sh" pg-proxy-3 replica_2
