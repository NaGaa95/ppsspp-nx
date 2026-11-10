#!/usr/bin/env python3
# Copyright (c) 2012- PPSSPP Project.
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2 or later.

"""Run the production depth worker with real kernels on one and several tile threads, and compare.

Under --thread-sanitizer, this also checks that tile threads never store to the same depth pixels."""
import argparse
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--thread-sanitizer', action='store_true')
args = parser.parse_args()
source = (root / 'GPU/Common/DepthRaster.cpp').read_text()

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
#include "Common/Thread/ThreadUtil.h"
#include "GPU/Common/DepthRaster.h"
#undef _dbg_assert_
#define _dbg_assert_(value) assert(value)

double time_now_d() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct {
    struct {
        int numDepthRasterPrims = 0, numDepthRasterNoPixels = 0, numDepthRasterTooSmall = 0;
        int numDepthRasterEarlySize = 0, numDepthRasterZCulled = 0, numDepthEarlyBoxCulled = 0;
    } perFrame;
} gpuStats;
constexpr int MEM_PROT_READ = 1, MEM_PROT_WRITE = 2;
void *AllocateMemoryPages(size_t size, int) { return malloc(size); }
void FreeMemoryPages(void *memory, size_t) { free(memory); }
void *AllocateAlignedMemory(size_t size, int align) {
    void *memory = nullptr;
    assert(posix_memalign(&memory, align, size) == 0);
    return memory;
}
void FreeAlignedMemory(void *memory) { free(memory); }
void SetCurrentThreadAffinity(ThreadAffinityRole) {}
void SetCurrentThreadName(const char *) {}
alignas(16) static u8 vram[0x200000];
namespace Memory {
bool IsVRAMAddress(u32 address) { return (address & 0x3F000000) == 0x04000000; }
bool IsValid4AlignedAddress(u32 address) { return IsVRAMAddress(address) && (address & 3) == 0; }
u8 *GetPointerWriteUnchecked(u32 address) { return vram + (address & 0x1FFFFF); }
}
'''
fixture += definition('DepthScissor DepthScissor::Tile(') + '\n'
fixture += definition('static void DepthRasterRect(') + '\n'
start = source.index('alignas(16) static const int zero123')
fixture += source[start:source.index('// A mix of ideas', start)]
fixture += 'template<ZCompareMode compareMode, bool lowQ>\n' + definition('void DepthRaster4Triangles(') + '\n'
for signature in ('int DepthRasterClipIndexedRectangles(', 'int DepthRasterClipIndexedTriangles(', 'int DepthRasterClip('):
    fixture += definition(signature) + '\n'
fixture += definition('void DepthRasterScreenVerts(') + '\n'
# The worker, through to the end of the file.
fixture += source[source.index('static constexpr u32 VRAM_OFFSET_MASK'):] + '\n'
fixture += r'''
struct Rng {
    u32 state = 0x2545F491;
    u32 Next() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }
    int Range(int lo, int hi) { return lo + (int)(Next() % (u32)(hi - lo + 1)); }
};

constexpr u32 DEPTH_ADDR = 0x04088000;
constexpr int STRIDE = 512;

// Several frames of batches with a mix of draws, the same sequence for each run.
std::vector<u16> Run(int tiles, int *draws) {
    Rng rng;
    memset(vram, 0, sizeof(vram));
    u16 *depth = (u16 *)Memory::GetPointerWriteUnchecked(DEPTH_ADDR);
    for (int i = 0; i < STRIDE * 272; ++i) {
        depth[i] = (u16)rng.Next();
    }
    {
        DepthRasterWorker worker(4096, 4096 * 3, tiles);
        for (int draw = 0; draw < 1500; ++draw) {
            const bool rects = draw % 11 == 0;
            const int prims = rng.Range(1, 60);
            const int vertsPerPrim = rects ? 2 : 3;
            const int verts = prims * vertsPerPrim;
            worker.Reserve(verts, verts);
            DepthBatch &batch = worker.Current();
            if (batch.draws.empty()) {
                batch.lowQ = rng.Next() % 4 == 0;
            }
            const int size = draw % 3 == 0 ? 8 : draw % 3 == 1 ? 60 : 250;
            float *out = batch.transformed + batch.vertexCount * 4;
            u16 *indices = batch.indices + batch.indexCount;
            for (int p = 0; p < prims; ++p) {
                const int cx = rng.Range(-40, 520);
                const int cy = rng.Range(-40, 300);
                for (int v = 0; v < vertsPerPrim; ++v) {
                    const int i = p * vertsPerPrim + v;
                    const float w = 0.5f + (rng.Next() % 1024) / 512.0f;
                    out[i * 4 + 0] = (float)(cx + rng.Range(-size, size)) * w;
                    out[i * 4 + 1] = (float)(cy + rng.Range(-size, size)) * w;
                    out[i * 4 + 2] = (float)rng.Range(1, 65535) * w;
                    out[i * 4 + 3] = w;
                    indices[i] = (u16)i;
                }
            }
            DepthDraw d{};
            // Now and then the same memory with another stride or address, which moves the tiles' columns.
            d.depthAddr = draw % 23 == 7 ? DEPTH_ADDR + 0x40 : DEPTH_ADDR;
            d.depthStride = draw % 17 == 5 ? 256 : STRIDE;
            d.prim = rects ? GE_PRIM_RECTANGLES : GE_PRIM_TRIANGLES;
            d.compareMode = rects ? ZCompareMode::Always : (ZCompareMode)(draw % 3);
            d.cullEnabled = draw % 2 == 0;
            d.cullMode = (draw / 2) % 2 ? GE_CULL_CW : GE_CULL_CCW;
            d.scissor = draw % 13 == 0 ? DepthScissor{ 200, 10, (u16)(200 + rng.Range(0, 9)), 250 } : DepthScissor{ 0, 0, 479, 271 };
            d.vertexOffset = batch.vertexCount;
            d.indexOffset = batch.indexCount;
            d.vertexCount = verts;
            batch.vertexCount += verts;
            batch.indexCount += verts;
            batch.draws.push_back(d);
            worker.SubmitIfReady();
            if (draw % 100 == 99) {
                worker.Wait();  // A list end.
            }
        }
        worker.Wait();
    }
    *draws += 1500;
    return std::vector<u16>(depth, depth + STRIDE * 272);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    int draws = 0;
    const std::vector<u16> reference = Run(1, &draws);
    int changed = 0;
    for (int tiles = 2; tiles <= 3; ++tiles) {
        const std::vector<u16> tiled = Run(tiles, &draws);
        int differing = 0;
        for (size_t i = 0; i < tiled.size(); ++i) {
            differing += tiled[i] != reference[i];
        }
        printf("%d tiles: %d pixels differ\n", tiles, differing);
        assert(differing == 0);
    }
    printf("%d draws\n", draws);
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='ppsspp-depth-tiles-') as directory:
    folder = Path(directory)
    cpp, executable = folder / 'test.cpp', folder / 'test'
    cpp.write_text(fixture)
    sanitizer = '-fsanitize=thread' if args.thread_sanitizer else '-fsanitize=address,undefined'
    subprocess.run(['clang++', '-std=c++17', '-O1', '-g', '-pthread', sanitizer,
                    '-fno-omit-frame-pointer', '-I' + str(root),
                    str(cpp), '-o', str(executable)], check=True)
    subprocess.run([str(executable), str(folder)], timeout=300, check=True)
    print('Depth tile checks passed: the worker with two and three tile threads writes the same depth buffer as one thread.')
