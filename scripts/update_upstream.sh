#!/usr/bin/env bash
set -euo pipefail
device=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
for repository in UniXNU UniXNU-sheng; do
  git -C "$device/../upstream/$repository" pull --quiet --ff-only --no-recurse-submodules
done
