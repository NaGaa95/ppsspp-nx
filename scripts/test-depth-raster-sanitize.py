#!/usr/bin/env python3
# Copyright (c) 2012- PPSSPP Project.
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2 or later.

"""Run production depth kernels and coverage/interpolation tests under ASan/UBSan."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'GPU/Common/DepthRaster.cpp').read_text()
test = (root / 'unittest/TestDepthRaster.cpp').read_text()

def definition(signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

fixture = r'''
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "ppsspp_config.h"
#include "Common/Math/CrossSIMD.h"
#include "GPU/Common/DepthRaster.h"
#include "unittest/UnitTest.h"
#undef _dbg_assert_
#define _dbg_assert_(value) assert(value)

double time_now_d() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct {
    struct {
        int numDepthRasterPrims = 0, numDepthRasterNoPixels = 0, numDepthRasterTooSmall = 0;
    } perFrame;
} gpuStats;
'''
fixture += definition('static void DepthRasterRect(') + '\n'
start = source.index('alignas(16) static const int zero123')
fixture += source[start:source.index('// A mix of ideas', start)]
fixture += 'template<ZCompareMode compareMode, bool lowQ>\n' + definition('void DepthRaster4Triangles(') + '\n'
fixture += definition('void DepthRasterScreenVerts(') + '\n'
fixture += test[test.index('namespace {'):]
fixture += '\nint main(int argc, char **argv) { assert(argc == 2); bool ok = TestDepthRaster(); return ok ? 0 : 1; }\n'

with tempfile.TemporaryDirectory(prefix='ppsspp-depth-raster-') as directory:
    folder = Path(directory)
    cpp, executable = folder / 'test.cpp', folder / 'test'
    cpp.write_text(fixture)
    subprocess.run(['clang++', '-std=c++17', '-O1', '-g', '-pthread',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-I' + str(root),
                    str(cpp), '-o', str(executable)], check=True)
    subprocess.run([str(executable), str(folder)], check=True)
    print('Depth sanitizer checks passed: production kernels, scalar coverage, exact interpolated output.')
