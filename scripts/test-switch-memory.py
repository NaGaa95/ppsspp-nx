#!/usr/bin/env python3
# Copyright (c) 2012- PPSSPP Project.
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2 or later.

"""Exercise the production Horizon arena with injected kernel/allocator failures."""
import argparse
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--baseline', help='Read production source from this Git revision')
args = parser.parse_args()
def source(path):
    if args.baseline:
        return subprocess.check_output(['git', 'show', args.baseline + ':' + path], cwd=root, text=True)
    return (root / path).read_text()

arena = source('Common/MemArenaHorizon.cpp')
arena = arena[arena.index('static uintptr_t memoryBase'):arena.rindex('#endif')]
mapping = source('Core/MemMap.cpp')
tail = mapping[mapping.index('\t// Should return true...'):mapping.index('\nvoid MemoryMap_Shutdown()')]
shutdown = mapping[mapping.index('void MemoryMap_Shutdown()'):mapping.index('// Commits the shadow under every view')]
ignore = mapping[mapping.index('static bool CanIgnoreView('):mapping.index('static bool SkipView(')]
fixture = (root / 'scripts/tests/SwitchMemoryArenaTest.cpp').read_text()
fixture = fixture.replace('// ARENA_SOURCE', arena).replace('// SETUP_TAIL', tail).replace('// IGNORE_VIEW', ignore).replace('// SHUTDOWN_SOURCE', shutdown)
with tempfile.TemporaryDirectory(prefix='ppsspp-switch-memory-') as directory:
    folder = Path(directory)
    cpp, executable = folder / 'test.cpp', folder / 'test'
    cpp.write_text(fixture)
    subprocess.run(['clang++', '-std=c++17', '-O1', '-g', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', str(cpp), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
