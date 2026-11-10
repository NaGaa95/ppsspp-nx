#!/usr/bin/env python3
# Copyright (c) 2012- PPSSPP Project.
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2 or later.

"""Check production depth worker submission/lifetime with a controlled raster callback."""
import argparse
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--thread-sanitizer', action='store_true')
args = parser.parse_args()
source = (root / 'GPU/Common/DepthRaster.cpp').read_text()
fixture = (root / 'scripts/tests/DepthRasterWorkerTest.cpp').read_text()
begin = source.index('static constexpr u32 VRAM_OFFSET_MASK')
end = source.index('void DepthRasterWorker::Rasterize(')
fixture = fixture.replace('// PRODUCTION_WORKER', source[begin:end])
with tempfile.TemporaryDirectory(prefix='ppsspp-depth-worker-') as directory:
    folder = Path(directory)
    cpp, executable = folder / 'test.cpp', folder / 'test'
    cpp.write_text(fixture)
    sanitizer = '-fsanitize=thread' if args.thread_sanitizer else '-fsanitize=address,undefined'
    subprocess.run(['clang++', '-std=c++17', '-g', '-O1', '-pthread', sanitizer,
                    '-fno-omit-frame-pointer', '-I' + str(root),
                    str(cpp), '-o', str(executable)], check=True)
    subprocess.run([str(executable), str(folder)], timeout=60, check=True)
    print('Depth worker checks passed: early overlap, FIFO results, vertex threshold, bounded queue backpressure, VRAM waits and destructor drain.')
