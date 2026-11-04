#!/usr/bin/env python3
# Copyright (c) 2012- PPSSPP Project.
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2 or later.

"""Check native input routing and launcher persistence against real core libraries.

Uses an existing Linux unit-test build, production method bodies and the real
ControlMapper, KeyMap and INI implementation. Only platform/UI services and
Horizon's no-replace rename are simulated. No game or console is required.
"""

import argparse
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


def body(source, marker):
    start = source.index("{", source.index(marker))
    depth, end = 1, start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=Path("build-wsl-tests"))
    parser.add_argument("--baseline-input")
    parser.add_argument("--baseline-save")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    build = args.build.resolve()
    def read(name, ref=None):
        return subprocess.check_output(["git", "show", ref + ":" + name], cwd=root, text=True) if ref else (root / name).read_text()
    native = read("UI/NativeApp.cpp", args.baseline_input)
    launcher = read("SDL/SwitchLauncher.cpp")
    keymap = read("Core/KeyMap.cpp")
    popups = read("Common/UI/PopupScreens.cpp")
    style = read("UI/SwitchUIStyle.cpp")
    route_start = native.index("InputMode inputMode =", native.index("bool NativeKey("))
    route_end = native.index("// Leave volume keys", route_start)
    axis_start = native.index("if (g_screenManager->PassInputToMapper()", native.index("void NativeAxis("))
    axis_end = native.index("QueuedEvent ev", axis_start)
    load_start = launcher.index('appearance_->Get("Classic"')
    load_end = launcher.index("for (auto &[key, value]", load_start)
    save = body(read("SDL/SwitchLauncher.cpp", args.baseline_save), "void Launcher::Save()") if args.baseline_save else body(launcher, "void Launcher::SaveAppearance()")
    save = save.replace('g_Config.Save("SwitchLauncher");', "").replace("File::Rename", "HorizonFiles::Rename")
    fixture = (root / "scripts/tests/SwitchUIStateTest.cpp").read_text()
    fixture = fixture.replace("// WIDGET_TESTS", (root / "scripts/tests/SwitchUIWidgetsTest.h").read_text())
    render_first = launcher.index("auto first = std::find(values.begin(), values.end(), (int)GPUBackend::DEKO3D)")
    render_start = launcher.rfind("if (ptr == &g_Config.iGPUBackend)", 0, render_first)
    replacements = {
        "KEY_ROUTE": native[route_start:route_end].replace("ImGui::", "RoutingImGui::"),
        "AXIS_ROUTE": native[axis_start:axis_end],
        "MENU_HELPERS": "\n".join("static void " + name + body(keymap, "static void " + name) for name in (
            "InsertIntoVector(std::vector<InputMapping> *vec, const InputMapping &mapping)",
            "RemoveKeyboardLetterKeys(std::vector<InputMapping> &mappings)")) +
            "\nbool HasMainButtonMapping(const std::vector<InputMapping> &mappings)" + body(keymap, "bool HasMainButtonMapping("),
        "MENU_KEYS": body(keymap, "void UpdateNativeMenuKeys()"),
        "WRITE_CONFIG": body(launcher, "bool WriteLauncherConfig(").replace("File::Rename", "HorizonFiles::Rename"),
        "LOAD_CONFIG": body(launcher, "bool LoadLauncherConfig("),
        "LOAD_APPEARANCE": launcher[load_start:load_end], "SAVE_APPEARANCE": save,
        "COLUMN_DEFAULT": re.search(r"int columns_\s*=\s*\d+;", launcher).group(),
        "POPUP_CLICK": body(popups, "void PopupMultiChoice::HandleClick("),
        "POPUP_CHOICE": body(popups, "void PopupMultiChoice::ChoiceCallback("),
        "FOOTER_DRAW": body(body(style, "class FooterView"), "void Draw(UIContext &dc)").replace("GetI18NCategory(", "FooterCategory("),
        "SDL_BACKEND_ORDER": body(launcher[render_start:], "if (ptr == &g_Config.iGPUBackend)"),
    }
    for name, value in replacements.items():
        fixture = fixture.replace("// " + name, value)
    def compile_source(original, source, output):
        target = "CMakeFiles/PPSSPPUnitTest.dir/unittest/UnitTest.cpp.o" if original.startswith("unittest/") else "Core/CMakeFiles/Core.dir/ControlMapper.cpp.o"
        lines = subprocess.check_output(["ninja", "-t", "commands", target], cwd=build, text=True).splitlines()
        command = shlex.split(lines[-1])
        for flag in ("-MT", "-MF"):
            if flag in command:
                index = command.index(flag)
                del command[index:index + 2]
        if "-MD" in command:
            command.remove("-MD")
        command.remove("-DNDEBUG")
        command[command.index("-o") + 1] = str(output)
        command[command.index("-c") + 1] = str(source)
        command += ["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, cwd=build, check=True)
    with tempfile.TemporaryDirectory(prefix="ppsspp-ui-state-") as directory:
        folder = Path(directory)
        source = folder / "test.cpp"
        source.write_text(fixture)
        obj = folder / "test.o"
        mapper = folder / "mapper.o"
        compile_source("unittest/UnitTest.cpp", source, obj)
        compile_source("Core/ControlMapper.cpp", root / "Core/ControlMapper.cpp", mapper)
        links = subprocess.check_output(["ninja", "-t", "commands", "PPSSPPUnitTest"], cwd=build, text=True).splitlines()
        words = shlex.split(next(line for line in reversed(links) if "-o PPSSPPUnitTest " in line))
        compiler = next(i for i, word in enumerate(words) if word.endswith(("clang++", "g++", "/c++")))
        end = words.index("&&", compiler) if "&&" in words[compiler:] else len(words)
        command = words[compiler:end]
        old = next(i for i, word in enumerate(command) if word.endswith("/unittest/UnitTest.cpp.o"))
        command[old:old + 1] = [str(obj), str(mapper)]
        executable = folder / "test"
        command[command.index("-o") + 1] = str(executable)
        command += ["-fsanitize=address,undefined"]
        subprocess.run(command, cwd=build, check=True)
        subprocess.run([str(executable), str(folder)], cwd=root, check=True)


if __name__ == "__main__":
    main()
