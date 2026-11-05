#!/usr/bin/env python3
# Copyright (c) 2012- PPSSPP Project.
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2 or later.

"""Check production binding caching through the real deko3d C++ SDK wrappers.

The C entry points record commands on a fake GPU; rendering still needs Switch testing.
"""
import argparse
import re
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--sdk-include', type=Path, required=True)
args = parser.parse_args()
source = (root / 'Common/GPU/Deko3D/DekoContext.cpp').read_text()

def definition(signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

fixture = (root / 'scripts/tests/DekoBindingsTest.cpp').read_text()
slots = re.search(r'constexpr uint32_t MAX_TEXTURE_SLOTS = (\d+);', (root / 'Common/GPU/thin3d.h').read_text()).group(1)
fixture = fixture.replace('// TEXTURE_SLOT_COUNT', f'constexpr unsigned MAX_TEXTURE_SLOTS = {slots};')
fixture = fixture.replace('// BINDING_IMPLEMENTATIONS', '\n'.join(
    definition(name) + ';' for name in ('struct Upload {', 'struct ResourceBindings {')))

# Command/descriptor resets and foreign GPU work must invalidate cached graphics state.
for signature in ('void Context::BeginFrame(', 'void Context::Submit(', 'void Context::ResetRenderBindings()',
                  'bool Context::DispatchScale(', 'void Context::Present(', 'void Invalidate(InvalidationFlags'):
    assert 'resourceBindings_.Invalidate();' in definition(signature), signature
assert source.count('frame.commands.clear();') == source.count('frame.commands.clear();\n' + '\t' +
    'resourceBindings_.Invalidate();') + source.count('frame.commands.clear();\n\t\tresourceBindings_.Invalidate();')
apply = definition('bool Context::Apply()')
assert 'resourceBindings_.Apply(cmd, handles, uniforms_);' in apply
assert apply.index('cmd.barrier(DkBarrier_None, DkInvalidateFlags_Descriptors);') < apply.index('resourceBindings_.Apply(')

with tempfile.TemporaryDirectory(prefix='ppsspp-deko-bindings-') as directory:
    cpp, executable = Path(directory) / 'test.cpp', Path(directory) / 'test'
    cpp.write_text(fixture)
    subprocess.run(['clang++', '-std=c++17', '-g', '-O1', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', '-I' + str(args.sdk_include), str(cpp), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
