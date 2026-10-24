#!/bin/bash
set -euo pipefail
base=$(cd "$(dirname "$0")/.." && pwd)
stack="$base/../UniXNU"
platform="$base/../marble_hackios"
edk2="$platform/Mu-Silicium/Mu_Basecore"
export WORKSPACE="$stack/out/marble/bootstrap"
export EDK_TOOLS_PATH="$edk2/BaseTools"
export CONF_PATH="$WORKSPACE/Build/Conf"
export PACKAGES_PATH="$base/package:$stack/package:$stack/drivers/DarwinBoot:$edk2"
export CLANG_BIN="$platform/bin/"
export PYTHON_COMMAND="$platform/venv/bin/python"
export PATH="$platform/venv/bin:$EDK_TOOLS_PATH/BinWrappers/PosixLike:$EDK_TOOLS_PATH/Source/C/bin:$PATH"
mkdir -p "$CONF_PATH"
for name in target tools_def build_rule; do
  cp "$EDK_TOOLS_PATH/Conf/$name.template" "$CONF_PATH/$name.txt"
done
build -p MarblePkg/MarbleBoot.dsc -a AARCH64 -b RELEASE -t CLANGPDB -n 4
