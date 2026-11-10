// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/TimeUtil.h"
#include "GPU/Common/DepthRaster.h"
#include "unittest/UnitTest.h"

namespace {

struct RasterRng {
	u32 state = 0x9E3779B9;
	u32 Next() {
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
		return state;
	}
	int Range(int lo, int hi) { return lo + (int)(Next() % (u32)(hi - lo + 1)); }
};

constexpr int BUF_W = 512;
constexpr int BUF_H = 280;

// Which pixels DepthRaster4Triangles covers, one pixel at a time: inside (or on) all three edges, within the
// triangle's bounds clipped to the scissor, with X widened to whole groups of four.
void ReferenceCoverage(std::vector<u8> &covered, int x0, int y0, int x1, int y1, int x2, int y2, const DepthScissor &scissor) {
	const int minX = std::max(std::min({ x0, x1, x2 }), (int)scissor.x1);
	const int maxX = std::min(std::max({ x0, x1, x2 }), (int)scissor.x2);
	const int minY = std::max(std::min({ y0, y1, y2 }), (int)scissor.y1);
	const int maxY = std::min(std::max({ y0, y1, y2 }), (int)scissor.y2);
	if (maxX <= minX || maxY <= minY)
		return;
	if ((x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0) < 10)
		return;
	for (int y = minY; y <= maxY; y++) {
		for (int x = minX & ~3; x <= (maxX & ~3) + 3; x++) {
			const int w0 = (y1 - y2) * x + (x2 - x1) * y + (x1 * y2 - y1 * x2);
			const int w1 = (y2 - y0) * x + (x0 - x2) * y + (x2 * y0 - y2 * x0);
			const int w2 = (y0 - y1) * x + (x1 - x0) * y + (x0 * y1 - y0 * x1);
			if ((w0 | w1 | w2) >= 0)
				covered[y * BUF_W + x] = 1;
		}
	}
}

// Clipping and rasterizing each tile's columns of the scissor on its own, as the worker's tile threads do,
// gives the same depth buffer as doing the whole scissor at once.
int CheckTiledDepth(RasterRng &rng, int *changedPixels) {
	constexpr int MAX_PRIMS = 40;
	constexpr int MAX_OUT = 1024;
	alignas(16) float transformed[MAX_PRIMS * 3 * 4];
	u16 indices[MAX_PRIMS * 3];
	alignas(16) int tx[MAX_OUT + 12];
	alignas(16) int ty[MAX_OUT + 12];
	alignas(16) float tz[MAX_OUT + 12];
	std::vector<u16> initial(BUF_W * BUF_H), whole, tiled;
	int mismatches = 0;
	for (int trial = 0; trial < 600; trial++) {
		DepthScissor scissor;
		if (trial % 5 == 0) {
			scissor.x1 = (u16)rng.Range(0, 400);
			scissor.x2 = (u16)(scissor.x1 + rng.Range(0, 12));
		} else {
			scissor.x1 = (u16)rng.Range(0, 100);
			scissor.x2 = (u16)rng.Range(380, 479);
		}
		scissor.y1 = (u16)rng.Range(0, 60);
		scissor.y2 = (u16)rng.Range(200, 270);
		DepthDraw draw{};
		draw.prim = trial % 7 == 0 ? GE_PRIM_RECTANGLES : GE_PRIM_TRIANGLES;
		draw.compareMode = draw.prim == GE_PRIM_RECTANGLES ? ZCompareMode::Always : (ZCompareMode)(trial % 3);
		draw.cullEnabled = trial % 2 == 0;
		draw.cullMode = (trial / 2) % 2 ? GE_CULL_CW : GE_CULL_CCW;
		draw.scissor = scissor;
		const bool lowQ = trial % 4 == 1;
		const int bufferWidth = trial % 2 ? 480 : 256;

		const int vertsPerPrim = draw.prim == GE_PRIM_RECTANGLES ? 2 : 3;
		const int prims = rng.Range(1, MAX_PRIMS);
		const int size = trial % 3 == 0 ? 8 : trial % 3 == 1 ? 60 : 300;
		for (int p = 0; p < prims; p++) {
			const int cx = rng.Range(-40, 520);
			const int cy = rng.Range(-40, 300);
			for (int v = 0; v < vertsPerPrim; v++) {
				const int i = p * vertsPerPrim + v;
				// Projected by the clipper, so scale by w. Some vertices are behind the camera.
				const float w = rng.Next() % 64 == 0 ? -1.0f : 0.5f + (rng.Next() % 1024) / 512.0f;
				transformed[i * 4 + 0] = (float)(cx + rng.Range(-size, size)) * w;
				transformed[i * 4 + 1] = (float)(cy + rng.Range(-size, size)) * w;
				transformed[i * 4 + 2] = (float)rng.Range(1, 65535) * w;
				transformed[i * 4 + 3] = w;
				indices[i] = (u16)i;
			}
		}
		draw.vertexCount = prims * vertsPerPrim;

		for (auto &value : initial) {
			value = (u16)rng.Next();
		}
		whole = initial;
		int count = DepthRasterClip(tx, ty, tz, transformed, indices, draw, scissor, MAX_OUT, false);
		DepthRasterScreenVerts(whole.data(), BUF_W, tx, ty, tz, count, draw, scissor, lowQ, false);
		for (size_t i = 0; i < whole.size(); i++) {
			*changedPixels += whole[i] != initial[i];
		}
		for (int numTiles : {2, 3}) {
			tiled = initial;
			for (int tile = 0; tile < numTiles; tile++) {
				const DepthScissor columns = scissor.Tile(tile, numTiles, bufferWidth);
				if (tile != 0 && columns.x1 > columns.x2) {
					continue;
				}
				count = DepthRasterClip(tx, ty, tz, transformed, indices, draw, columns, MAX_OUT, false);
				DepthRasterScreenVerts(tiled.data(), BUF_W, tx, ty, tz, count, draw, scissor, lowQ, false, true, &columns);
			}
			if (tiled != whole) {
				if (mismatches++ < 10) {
					printf("Tiled depth differs: trial=%d tiles=%d prim=%d compare=%d lowQ=%d scissor=%d-%d\n", trial, numTiles, (int)draw.prim, (int)draw.compareMode, lowQ, scissor.x1, scissor.x2);
				}
			}
		}
	}
	return mismatches;
}

}  // namespace

// The depth rasterizer covers exactly the pixels a per-pixel edge test does. All triangles are at one depth, so
// with a cleared buffer and Greater, the written pixels are the covered ones. (Low quality is left out: it
// copies each row's blocks to the next row, along with what was there.)
bool TestDepthRaster() {
	RasterRng rng;
	std::vector<u16> depth(BUF_W * BUF_H);
	std::vector<u8> covered(BUF_W * BUF_H);
	int mismatches = 0;
	int pixels = 0;
	int interpolatedCases = 0;
	std::vector<u16> reference(depth.size()), skipped(depth.size());

	for (int trial = 0; trial < 4000; trial++) {
		DepthScissor scissor;
		scissor.x1 = (u16)rng.Range(0, 100);
		scissor.y1 = (u16)rng.Range(0, 60);
		scissor.x2 = (u16)rng.Range(380, 480);
		scissor.y2 = (u16)rng.Range(200, 270);
		DepthDraw draw{};
		draw.prim = GE_PRIM_TRIANGLES;
		draw.compareMode = ZCompareMode::Greater;

		// Groups of four triangles: x, y and z of vertex 0 of each, then vertex 1, then vertex 2.
		const int groups = 1 + rng.Range(0, 3);
		alignas(16) int tx[48];
		alignas(16) int ty[48];
		alignas(16) float tz[48];
		std::fill(depth.begin(), depth.end(), 0);
		std::fill(covered.begin(), covered.end(), 0);
		// Small and thin triangles as well as big ones, some partly off the scissor.
		const int size = trial % 3 == 0 ? 8 : trial % 3 == 1 ? 60 : 400;
		for (int g = 0; g < groups; g++) {
			for (int t = 0; t < 4; t++) {
				const int cx = rng.Range(-40, 520);
				const int cy = rng.Range(-40, 300);
				int x[3], y[3];
				for (int v = 0; v < 3; v++) {
					x[v] = cx + rng.Range(-size, size);
					y[v] = cy + rng.Range(-size, size);
					tx[g * 12 + v * 4 + t] = x[v];
					ty[g * 12 + v * 4 + t] = y[v];
					tz[g * 12 + v * 4 + t] = 1000.0f;
				}
				ReferenceCoverage(covered, x[0], y[0], x[1], y[1], x[2], y[2], scissor);
			}
		}
		DepthRasterScreenVerts(depth.data(), BUF_W, tx, ty, tz, groups * 12, draw, scissor, false);

		for (int i = 0; i < BUF_W * BUF_H; i++) {
			const bool written = depth[i] != 0;
			pixels += written;
			if (written != (covered[i] != 0) || (written && depth[i] != 1000)) {
				if (mismatches++ < 10)
					printf("Trial %d: pixel %d,%d is %d, expected %s\n", trial, i % BUF_W, i / BUF_W, depth[i], covered[i] ? "covered" : "empty");
			}
		}
		if (trial < 200) {
			if (trial % 10 == 0) {
				tx[4] = 10000;
			}
			if (trial % 17 == 0) {
				ty[8] = -10000;
			}
			for (int i = 0; i < groups * 12; ++i) {
				tz[i] = (rng.Next() & 0xFFFF) + (rng.Next() & 255) * (1.0f / 256.0f);
			}
			for (bool lowQ : {false, true}) {
				for (ZCompareMode compare : {ZCompareMode::Greater, ZCompareMode::Less, ZCompareMode::Always}) {
					for (auto &value : reference) {
						value = (u16)rng.Next();
					}
					skipped = reference;
					draw.compareMode = compare;
					DepthRasterScreenVerts(reference.data(), BUF_W, tx, ty, tz, groups * 12, draw, scissor, lowQ, false, false);
					DepthRasterScreenVerts(skipped.data(), BUF_W, tx, ty, tz, groups * 12, draw, scissor, lowQ, false, true);
					if (reference != skipped) {
						printf("Empty-block skip changed depth: trial=%d lowQ=%d compare=%d\n", trial, lowQ, (int)compare);
						mismatches++;
					}
					++interpolatedCases;
				}
			}
		}
	}
	printf("%d pixels covered; %d interpolated depth buffers match the original traversal\n", pixels, interpolatedCases);
	EXPECT_EQ_INT(interpolatedCases, 1200);
	int tiledChanged = 0;
	const int tiledMismatches = CheckTiledDepth(rng, &tiledChanged);
	printf("Tiled depth raster: %d mismatches, %d pixels written\n", tiledMismatches, tiledChanged);
	EXPECT_EQ_INT(tiledMismatches, 0);
	EXPECT_TRUE(tiledChanged > 1000000);
	if (getenv("PPSSPP_BENCH_DEPTH")) {
		DepthDraw draw{};
		draw.prim = GE_PRIM_TRIANGLES;
		draw.compareMode = ZCompareMode::Greater;
		const DepthScissor scissor{0, 0, 479, 271};
		for (int shape = 0; shape < 3; ++shape) {
			alignas(16) int tx[12], ty[12];
			alignas(16) float tz[12];
			for (int t = 0; t < 4; ++t) {
				tx[t] = 8; ty[t] = 8;
				tx[t + 4] = shape == 2 ? 16 : 475; ty[t + 4] = shape == 0 ? 264 : 8;
				tx[t + 8] = shape == 0 ? 472 : 8; ty[t + 8] = shape == 2 ? 16 : 264;
				tz[t] = 1000.25f; tz[t + 4] = 40000.5f; tz[t + 8] = 20000.75f;
			}
			for (bool skip : {false, true}) {
				const double rate = CallsPerSecond([&] {
					DepthRasterScreenVerts(depth.data(), BUF_W, tx, ty, tz, 12, draw, scissor, false, false, skip);
				}, 0.3, 100);
				printf("Depth benchmark shape=%d skip=%d: %.0f triangles/s\n", shape, skip, rate * 4);
			}
		}
	}
	EXPECT_EQ_INT(mismatches, 0);
	EXPECT_TRUE(pixels > 1000000);
	return true;
}
