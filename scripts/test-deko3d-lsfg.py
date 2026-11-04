#!/usr/bin/env python3
# Copyright (c) 2012- PPSSPP Project.
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2 or later.

"""Validate native LSFG shaders and runtime against an installed SDK and DLL.

Run on Linux with a DEKO3D_SHADER_TESTS build. This uses the actual SDK headers,
the actual frame generator and a GPU test double, not a Switch GPU emulator.
Lossless.dll and generated shader caches are never bundled in the NRO.
"""

import argparse
import os
from pathlib import Path
import shlex
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dll", type=Path, required=True)
    parser.add_argument("--sdk-include", type=Path, default=Path("/opt/devkitpro/libnx/include"))
    parser.add_argument("--build", type=Path, default=Path("build-wsl-tests"))
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    build = args.build.resolve()
    assert (args.sdk_include / "deko3d.hpp").is_file()
    assert args.dll.is_file()
    subprocess.run(["cmake", "--build", str(build), "--target", "SwitchLsfgDekoPrepare", "--parallel", "6"], check=True)
    compiler = shlex.split(os.environ.get("CXX", "clang++"))
    executable = root / "build-switch-deps/DekoLSFGTest"
    libraries = [build / "lib" / ("lib" + name + ".a") for name in
                 ("SwitchLsfgDekoPrepare", "SwitchLsfgDekoShaders", "spirv-cross-glsl", "spirv-cross-core")]
    subprocess.run(compiler + ["-std=c++20", "-O1", "-g", "-pthread", "-fsanitize=address,undefined",
                              "-fno-omit-frame-pointer", "-I", str(root), "-I", str(args.sdk_include),
                              "-I", str(root / "ext/lsfg-deko3d/include"),
                              str(root / "scripts/tests/DekoLSFGTest.cpp"),
                              str(root / "Common/GPU/Deko3D/LSFGFrameGenerator.cpp"),
                              *map(str, libraries), str(build / "uam-generated/uam_isolated.o"),
                              "-o", str(executable)], check=True)
    subprocess.run([str(executable), str(args.dll), str(root / "build-switch-deps/lsfg-native-test-cache")], check=True)


if __name__ == "__main__":
    main()
