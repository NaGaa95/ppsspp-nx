#!/usr/bin/env python3
# Copyright (c) 2012- PPSSPP Project.
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2 or later.

"""Exercise actual LSFG controls, asynchronous setup and presentation with a fake GPU.

This complements test-deko3d-lsfg.py's real shader/SDK runtime checks. It does not
execute GPU shaders or validate interpolation quality on Switch hardware.
"""

import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


def function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    root = Path(__file__).resolve().parent.parent
    controls = (root / "Common/GPU/Vulkan/SwitchLSFG.cpp").read_text()
    context = (root / "Common/GPU/Deko3D/DekoContext.cpp").read_text()
    functions = "\n".join(function(controls, signature) for signature in (
        "void SwitchLSFG_Configure(", "void SwitchLSFG_SetFastForward(", "SwitchLSFGDekoState SwitchLSFG_PollDeko()",
        "void SwitchLSFG_SetDekoStatus(", "bool SwitchLSFG_IsPrepared()",
        "bool SwitchLSFG_IsInstalled()", "bool SwitchLSFG_IsAvailable()",
        "bool SwitchLSFG_IsEnabled()", "bool SwitchLSFG_IsHighRatePassthrough()",
        "bool SwitchLSFG_RequestEnabled(", "void SwitchLSFG_ResetSession()"))
    methods = "\n".join(function(context, signature) for signature in (
        "void Context::UpdateFrameGeneration()", "bool Context::PresentImage(",
        "void Context::Present(PresentMode mode)"))
    # Keep all production pacing logic; substitute only its wall-clock input.
    methods, count = re.subn(r"const auto now = std::chrono::duration_cast<.*?;", "const auto now = nowNs_;", methods)
    assert count == 1
    fixture = (root / "scripts/tests/DekoLSFGContextTest.cpp").read_text()
    fixture = fixture.replace("// CONTROL_IMPLEMENTATIONS", functions).replace("// CONTEXT_IMPLEMENTATIONS", methods)
    with tempfile.TemporaryDirectory(prefix="ppsspp-lsfg-context-") as directory:
        folder = Path(directory)
        source = folder / "test.cpp"
        source.write_text(fixture)
        executable = folder / "test"
        compiler = shlex.split(os.environ.get("CXX", "clang++"))
        subprocess.run(compiler + ["-std=c++17", "-O1", "-g", "-pthread", "-fsanitize=address,undefined",
                                  "-fno-omit-frame-pointer", "-I", str(root), str(source), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
