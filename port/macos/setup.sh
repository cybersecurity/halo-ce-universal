#!/bin/bash
set -euo pipefail
port_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source_root=$(cd "$port_dir/../.." && pwd)
exec bash "$port_dir/tooling/setup.sh" "$@" --source-checkout "$source_root"
