#!/usr/bin/env python3
# Copyright (c) 2012- PPSSPP Project.
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2 or later.

"""Check LSFG fast-forward using production timing/presentation and a fake Vulkan GPU."""

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


def function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-switch", help="Reproduce the Vulkan bypass failure from an older revision")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    controls = (root / "Common/GPU/Vulkan/SwitchLSFG.cpp").read_text()
    bridge = (root / "Common/GPU/Vulkan/lsfg/lsfg_bridge.cpp").read_text()
    timing = (root / "Core/FrameTiming.cpp").read_text()
    thin3d = (root / "Common/GPU/Vulkan/thin3d_vulkan.cpp").read_text()
    old = subprocess.check_output(["git", "show", args.baseline_switch + ":Common/GPU/Vulkan/SwitchLSFG.cpp"],
                                  cwd=root, text=True) if args.baseline_switch else controls
    globals_begin = re.search(r"constexpr const char\s*\*\s*kShaderDllPath", controls).start()
    globals_end = controls.index("bool FileReadable(")
    implementations = "\n".join(function(controls, signature) for signature in (
        "void DestroyRuntime()", "void ResetTiming()", "void SwitchLSFG_Configure(",
        "void SwitchLSFG_SetFastForward(", "bool SwitchLSFG_IsEnabled()",
        "bool SwitchLSFG_IsHighRatePassthrough()", "bool SwitchLSFG_RequestEnabled(",
        "uint64_t ObserveSourcePresent()", "bool SourceIsHighRate()"))
    implementations += "\n" + function(old, "bool SwitchLSFG_Present(")
    fields_start = re.search(r"std::array<bool,\s*2>\s+sourceInitialized", bridge).start()
    fields_end = fields_start + re.search(r"\bsize_t\s+slotCursor\s*\{\};", bridge[fields_start:]).end()
    bridge_methods = "\n".join(function(bridge, signature) for signature in (
        "void resetHistory()", "VkResult present(VkQueue", re.search(r"FrameSlot\s*&\s*prepareSlot\(\)", bridge).group(),
        "VkResult presentGenerated(", "VkResult presentOriginal("))
    fixture = (root / "scripts/tests/SwitchLSFGSpeedTest.cpp").read_text()
    replacements = {
        "GLOBALS": controls[globals_begin:globals_end], "CONTROLS": implementations,
        "TIMING": function(timing, "void FrameTiming::ComputePresentMode("),
        "BRIDGE_FIELDS": bridge[fields_start:fields_end], "BRIDGE_METHODS": bridge_methods,
        "RESET_EXPORT": function(bridge, 'extern "C" void lsfg_nx_reset_history('),
        "PRESENT_CAPS": thin3d[thin3d.index("caps_.presentMaxInterval = 1;"):thin3d.index("const auto &limits =")],
    }
    for name, value in replacements.items():
        fixture = fixture.replace("// " + name, value)
    with tempfile.TemporaryDirectory(prefix="ppsspp-lsfg-speed-") as directory:
        source = Path(directory) / "test.cpp"
        source.write_text(fixture)
        executable = Path(directory) / "test"
        compiler = shlex.split(os.environ.get("CXX", "clang++"))
        subprocess.run(compiler + ["-std=c++20", "-O1", "-g", "-pthread", "-fsanitize=address,undefined",
                                  "-fno-omit-frame-pointer", "-I", str(root), "-I", str(root / "ext"),
                                  str(source), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
