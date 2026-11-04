#!/usr/bin/env bash
set -euo pipefail
cd "$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)"
mkdir -p build-switch-deps
clang++ -std=c++17 -O2 -ffunction-sections -fdata-sections -Wl,--gc-sections -I. $(pkg-config --cflags freetype2 harfbuzz fribidi) SDL/SwitchText.cpp SDL/tests/SwitchTextTest.cpp Common/Data/Encoding/Utf8.cpp $(pkg-config --libs freetype2 harfbuzz fribidi glib-2.0) -o build-switch-deps/SwitchTextTest
build-switch-deps/SwitchTextTest assets/launcher/fonts build-switch-deps/font-shaping-preview.bmp assets/lang "$@"
