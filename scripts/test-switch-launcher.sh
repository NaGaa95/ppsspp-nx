#!/usr/bin/env bash
set -euo pipefail
cd "$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)"
mkdir -p build-switch-deps
clang++ -std=c++17 -Wall -Wextra -Werror -I. SDL/tests/SwitchLauncherTest.cpp -o build-switch-deps/SwitchLauncherTest
build-switch-deps/SwitchLauncherTest
