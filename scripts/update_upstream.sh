#!/usr/bin/env bash
set -euo pipefail
device=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
exec "$device/../../tools/marble-build/venv/bin/python" "$device/scripts/update_upstream.py" "$@"
