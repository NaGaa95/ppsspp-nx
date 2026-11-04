#!/usr/bin/env python3
# Copyright (c) 2012- PPSSPP Project.
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2 or later.

"""Exercise the production Horizon arena with injected kernel/allocator failures."""
import argparse
from pathlib import Path
import subprocess
import tempfile
import re

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
shutdown = mapping[mapping.index('void MemoryMap_Shutdown()'):mapping.index('// Commits the shadow under every view')]
ignore = mapping[mapping.index('static bool CanIgnoreView('):mapping.index('static bool SkipView(')]
skip = mapping[mapping.index('static bool SkipView('):mapping.index('#if defined(__APPLE__)', mapping.index('static bool SkipView('))]
try_base = mapping[mapping.index('static bool Memory_TryBase('):mapping.index('bool MemoryMap_Setup(')]
setup = mapping[mapping.index('bool MemoryMap_Setup('):mapping.index('void MemoryMap_Shutdown()')]
header = source('Core/MemMap.h')
types = header[header.index('enum {\n\tMV_MIRROR_PREVIOUS'):header.index('ENUM_CLASS_BITOPS(MemMapSetupFlags);')]
table = mapping[mapping.index('static MemoryView views[]'):mapping.index('\n};', mapping.index('static MemoryView views[]')) + 3]
indices = iter(range(128))
table = re.sub(r'&m_\w+(?:\[\d+\])?', lambda _: '&pointers[' + str(next(indices)) + ']', table)
start = mapping.index('\tfor (size_t i = 0; i < ARRAY_SIZE(views); i++)', mapping.index('bool Init(MemMapSetupFlags flags)'))
configure = mapping[start:mapping.index('\n\t// The shadow first', start)]
fixture = (root / 'scripts/tests/SwitchMemoryRangeTest.cpp').read_text()
replacements = {'ARENA_SOURCE': arena, 'VIEW_TYPES': types, 'VIEW_TABLE': table, 'IGNORE_VIEW': ignore,
                'SKIP_VIEW': skip, 'TRY_BASE_SOURCE': try_base, 'SETUP_SOURCE': setup,
                'SHUTDOWN_SOURCE': shutdown, 'CONFIGURE_VIEWS': configure,
                'BASELINE_API': '' if 'void MemArena::ReleaseViewSpace(' in arena else 'void MemArena::ReleaseViewSpace(u8 *) {}'}
for name, replacement in replacements.items():
    fixture = fixture.replace('// ' + name, replacement)
with tempfile.TemporaryDirectory(prefix='ppsspp-switch-memory-') as directory:
    folder = Path(directory)
    cpp, executable = folder / 'test.cpp', folder / 'test'
    cpp.write_text(fixture)
    for masked in (False, True):
        subprocess.run(['clang++', '-std=c++17', '-O1', '-g', '-fsanitize=address,undefined',
                        '-fno-omit-frame-pointer', *(['-DTEST_MASKED'] if masked else []),
                        str(cpp), '-o', str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
