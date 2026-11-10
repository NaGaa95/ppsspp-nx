#include <algorithm>
#include <cstring>
#include <cstdint>

#include "Common/Math/CrossSIMD.h"
#include "GPU/Common/DepthRaster.h"
#include "GPU/Math3D.h"
#include "Common/Math/math_util.h"
#include "GPU/Common/VertexDecoderCommon.h"
#include "Common/MemoryUtil.h"
#include "Common/Thread/ThreadUtil.h"
#include "Common/TimeUtil.h"
#include "Core/MemMap.h"

DepthScissor DepthScissor::Tile(int tile, int numTiles, int bufferWidth, int split) const {
	if (numTiles <= 1) {
		return *this;
	}
	_dbg_assert_(tile >= 0 && tile < numTiles);

	// The buffer is split at fixed columns (whole groups of four), so a pixel belongs to the same tile in
	// every draw to it, whatever the scissor.
	auto boundary = [&](int t) {
		const int x = numTiles == 2 ? bufferWidth * split / DEPTH_SPLIT_ONE : bufferWidth * t / numTiles;
		return (x + 3) & ~3;
	};
	const int start = tile == 0 ? 0 : boundary(tile);
	const int endExclusive = tile == numTiles - 1 ? 0x10000 : boundary(tile + 1);

	DepthScissor scissor = *this;
	scissor.x1 = (u16)std::max((int)x1, start);
	scissor.x2 = (u16)std::min((int)x2, endExclusive - 1);
	return scissor;  // x1 > x2 if the scissor misses these columns.
}

// x1/x2 etc are the scissor rect.
static void DepthRasterRect(uint16_t *dest, int stride, const DepthScissor scissor, int v1x, int v1y, int v2x, int v2y, short depthValue, ZCompareMode compareMode) {
	// Swap coordinates if needed, we don't back-face-cull rects.
	// We also ignore the UV rotation here.
	if (v1x > v2x) {
		std::swap(v1x, v2x);
	}
	if (v1y > v2y) {
		std::swap(v1y, v2y);
	}

	if (v1x < scissor.x1) {
		v1x = scissor.x1;
	}
	if (v2x > scissor.x2) {
		v2x = scissor.x2 + 1;  // PSP scissors are inclusive
	}
	if (v1x >= v2x) {
		return;
	}

	if (v1y < scissor.y1) {
		v1y = scissor.y1;
	}
	if (v2y > scissor.y2) {
		v2y = scissor.y2 + 1;
	}
	if (v1y >= v2y) {
		return;
	}

	Vec8U16 valueX8 = Vec8U16::Splat(depthValue);
	for (int y = v1y; y < v2y; y++) {
		uint16_t *ptr = (uint16_t *)(dest + stride * y + v1x);
		int w = v2x - v1x;
		switch (compareMode) {
		case ZCompareMode::Always:
			if (depthValue == 0) {
				memset(ptr, 0, w * 2);
			} else {
				while (w >= 8) {
					valueX8.Store(ptr);
					ptr += 8;
					w -= 8;
				}
				// Non-simd trailer.
				while (w > 0) {
					*ptr++ = depthValue;
					w--;
				}
			}
			break;
		default:
			// TODO
			break;
		}
	}
}

alignas(16) static const int zero123[4] = {0, 1, 2, 3};

enum class TriangleStat {
	OK,
	NoPixels,
	SmallOrBackface,
};

constexpr int MIN_TWICE_TRI_AREA = 10;

// A mix of ideas from Intel's sample and ryg's rasterizer blog series.
// Only the groups of four pixels from tileX1 to tileX2 (multiples of four) are written.
template<ZCompareMode compareMode, bool lowQ>
void DepthRaster4Triangles(int stats[3], uint16_t *depthBuf, int stride, DepthScissor scissor, int tileX1, int tileX2, const int *tx, const int *ty, const float *tz, bool skipEmptyBlocks) {
	// Triangle setup. This is done using SIMD, four triangles at a time.
	// 16x16->32 multiplications are doable on SSE2, which should be all we need.

	// We use 4x1 SIMD tiles for simplicity. 2x2 would be ideal but stores/loads get annoying.

	// NOTE: Triangles are stored in groups of 4.
	Vec4S32 x0 = Vec4S32::LoadAligned(tx);
	Vec4S32 y0 = Vec4S32::LoadAligned(ty);
	Vec4S32 x1 = Vec4S32::LoadAligned(tx + 4);
	Vec4S32 y1 = Vec4S32::LoadAligned(ty + 4);
	Vec4S32 x2 = Vec4S32::LoadAligned(tx + 8);
	Vec4S32 y2 = Vec4S32::LoadAligned(ty + 8);

	if (lowQ) {
		y0 &= Vec4S32::Splat(~1);
		y1 &= Vec4S32::Splat(~1);
		y2 &= Vec4S32::Splat(~1);
	}

	// FixupAfterMinMax is just 16->32 sign extension, in case the current platform (like SSE2) just has 16-bit min/max operations.
	Vec4S32 minX = x0.Min16(x1).Min16(x2).Max16(Vec4S32::Splat(scissor.x1)).FixupAfterMinMax();
	Vec4S32 maxX = x0.Max16(x1).Max16(x2).Min16(Vec4S32::Splat(scissor.x2)).FixupAfterMinMax();
	Vec4S32 minY = y0.Min16(y1).Min16(y2).Max16(Vec4S32::Splat(scissor.y1)).FixupAfterMinMax();
	Vec4S32 maxY = y0.Max16(y1).Max16(y2).Min16(Vec4S32::Splat(scissor.y2)).FixupAfterMinMax();

	Vec4S32 triArea = (x1 - x0).Mul16(y2 - y0) - (x2 - x0).Mul16(y1 - y0);

	// Edge setup
	Vec4S32 A12 = y1 - y2;
	Vec4S32 B12 = x2 - x1;
	Vec4S32 C12 = x1.Mul16(y2) - y1.Mul16(x2);

	Vec4S32 A20 = y2 - y0;
	Vec4S32 B20 = x0 - x2;
	Vec4S32 C20 = x2.Mul16(y0) - y2.Mul16(x0);

	Vec4S32 A01 = y0 - y1;
	Vec4S32 B01 = x1 - x0;
	Vec4S32 C01 = x0.Mul16(y1) - y0.Mul16(x1);

	constexpr int stepXSize = 4;
	constexpr int stepYSize = lowQ ? 2 : 1;

	constexpr int stepXShift = 2;
	constexpr int stepYShift = lowQ ? 1 : 0;

	// Step deltas
	Vec4S32 stepX12 = A12.Shl<stepXShift>();
	Vec4S32 stepY12 = B12.Shl<stepYShift>();
	Vec4S32 stepX20 = A20.Shl<stepXShift>();
	Vec4S32 stepY20 = B20.Shl<stepYShift>();
	Vec4S32 stepX01 = A01.Shl<stepXShift>();
	Vec4S32 stepY01 = B01.Shl<stepYShift>();

	// Prepare to interpolate Z
	Vec4F32 oneOverTriArea = Vec4F32FromS32(triArea).Recip();
	Vec4F32 zbase = Vec4F32::LoadAligned(tz);
	Vec4F32 z_20 = (Vec4F32::LoadAligned(tz + 4) - zbase) * oneOverTriArea;
	Vec4F32 z_01 = (Vec4F32::LoadAligned(tz + 8) - zbase) * oneOverTriArea;
	Vec4F32 zdx = z_20 * Vec4F32FromS32(stepX20) + z_01 * Vec4F32FromS32(stepX01);
	Vec4F32 zdy = z_20 * Vec4F32FromS32(stepY20) + z_01 * Vec4F32FromS32(stepY01);

	// Shared setup is done, now loop per-triangle in the group of four.
	for (int t = 0; t < 4; t++) {
		// Check for bad triangle.
		// Using operator[] on the vectors actually seems to result in pretty good code.
		if (maxX[t] <= minX[t] || maxY[t] <= minY[t]) {
			// No pixels, or outside screen.
			// Most of these are now gone in the initial pass, but not all since we cull
			// in 4-groups there.
			stats[(int)TriangleStat::NoPixels]++;
			continue;
		}

		if (triArea[t] < MIN_TWICE_TRI_AREA) {
			stats[(int)TriangleStat::SmallOrBackface]++;  // Or zero area.
			continue;
		}
		stats[(int)TriangleStat::OK]++;

		const int minXT = minX[t] & ~3;
		const int maxXT = std::min(maxX[t] & ~3, tileX2);
		// Where this tile's part of the rows starts, and how many groups the whole triangle has before that.
		const int startXT = std::max(minXT, tileX1);
		const int skippedGroups = (startXT - minXT) >> 2;
		if (startXT > maxXT) {
			continue;
		}

		const int minYT = minY[t];
		const int maxYT = maxY[t];
		// The coarse edge bounds must not cross a signed edge-function overflow.
		auto inGuard = [](int value) { return (u32)value + 4096U <= 8192U; };
		const bool canSkip = skipEmptyBlocks && (u32)maxXT - (u32)startXT >= 28 &&
			inGuard(x0[t]) && inGuard(x1[t]) && inGuard(x2[t]) &&
			inGuard(y0[t]) && inGuard(y1[t]) && inGuard(y2[t]);
		const int maxAhead12 = canSkip ? std::max(stepX12[t] * 7, 0) : 0;
		const int maxAhead20 = canSkip ? std::max(stepX20[t] * 7, 0) : 0;
		const int maxAhead01 = canSkip ? std::max(stepX01[t] * 7, 0) : 0;

		// Convert to wide registers.
		Vec4S32 initialX = Vec4S32::Splat(minXT) + Vec4S32::LoadAligned(zero123);
		int initialY = minY[t];
		_dbg_assert_(A12[t] < 32767);
		_dbg_assert_(A12[t] > -32767);
		_dbg_assert_(A20[t] < 32767);
		_dbg_assert_(A20[t] > -32767);
		_dbg_assert_(A01[t] < 32767);
		_dbg_assert_(A01[t] > -32767);

		// TODO: The latter subexpression can be broken out of this loop, but reduces block size flexibility.
		Vec4S32 w0_row = Vec4S32::Splat(A12[t]).Mul16(initialX) + Vec4S32::Splat(B12[t] * initialY + C12[t]);
		Vec4S32 w1_row = Vec4S32::Splat(A20[t]).Mul16(initialX) + Vec4S32::Splat(B20[t] * initialY + C20[t]);
		Vec4S32 w2_row = Vec4S32::Splat(A01[t]).Mul16(initialX) + Vec4S32::Splat(B01[t] * initialY + C01[t]);

		Vec4F32 zrow = Vec4F32::Splat(zbase[t]) + Vec4F32FromS32(w1_row) * z_20[t] + Vec4F32FromS32(w2_row) * z_01[t];
		Vec4F32 zdeltaX = Vec4F32::Splat(zdx[t]);
		Vec4F32 zdeltaY = Vec4F32::Splat(zdy[t]);
		// The edge functions are exact, so they can jump to the tile's first group.
		w0_row += Vec4S32::Splat(stepX12[t] * skippedGroups);
		w1_row += Vec4S32::Splat(stepX20[t] * skippedGroups);
		w2_row += Vec4S32::Splat(stepX01[t] * skippedGroups);

		Vec4S32 oneStepX12 = Vec4S32::Splat(stepX12[t]);
		Vec4S32 oneStepY12 = Vec4S32::Splat(stepY12[t]);
		Vec4S32 oneStepX20 = Vec4S32::Splat(stepX20[t]);
		Vec4S32 oneStepY20 = Vec4S32::Splat(stepY20[t]);
		Vec4S32 oneStepX01 = Vec4S32::Splat(stepX01[t]);
		Vec4S32 oneStepY01 = Vec4S32::Splat(stepY01[t]);
		// Rasterize. The triangle is convex, so once a row has been entered and left, the rest of it is outside.
		for (int y = minYT; y <= maxYT; y += stepYSize, w0_row += oneStepY12, w1_row += oneStepY20, w2_row += oneStepY01, zrow += zdeltaY) {
			// Barycentric coordinates at start of row
			Vec4S32 w0 = w0_row;
			Vec4S32 w1 = w1_row;
			Vec4S32 w2 = w2_row;
			Vec4F32 zs = zrow;
			// Depth isn't, so repeat the whole row's additions (and their rounding).
			for (int step = 0; step < skippedGroups; ++step) {
				zs += zdeltaX;
			}

			uint16_t *rowPtr = depthBuf + stride * y;

			bool rowEntered = false;
			for (int x = startXT; x <= maxXT; x += stepXSize, w0 += oneStepX12, w1 += oneStepX20, w2 += oneStepX01, zs += zdeltaX) {
				// If p is on or inside all edges for any pixels,
				// render those pixels.
				Vec4S32 signCalc = w0 | w1 | w2;

				if (!AnyZeroSignBit(signCalc)) {
					if (rowEntered) {
						break;
					}
					if (canSkip && x + 28 <= maxXT) {
						// Bound each lane over eight tiles. Different edges may reach their maxima at
						// different X positions; this overestimates coverage and can only miss a skip.
						const Vec4S32 coarse = (w0 + Vec4S32::Splat(maxAhead12)) |
							(w1 + Vec4S32::Splat(maxAhead20)) | (w2 + Vec4S32::Splat(maxAhead01));
						if (!AnyZeroSignBit(coarse)) {
							w0 += oneStepX12.Shl<3>() - oneStepX12;
							w1 += oneStepX20.Shl<3>() - oneStepX20;
							w2 += oneStepX01.Shl<3>() - oneStepX01;
							// Keep the original sequence of float additions, including rounding.
							for (int step = 0; step < 7; ++step) {
								zs += zdeltaX;
							}
							x += 28;  // The loop increment advances the eighth tile.
						}
					}
					continue;
				}
				rowEntered = true;

				Vec4U16 bufferValues = Vec4U16::Load(rowPtr + x);
				Vec4U16 shortMaskInv = SignBits32ToMaskU16(signCalc);
				// Now, the mask has 1111111 where we should preserve the contents of the depth buffer.

				Vec4U16 shortZ = Vec4U16::FromVec4F32(zs);

				// This switch is on a templated constant, so should collapse away.
				Vec4U16 writeVal;
				switch (compareMode) {
				case ZCompareMode::Greater:
					// To implement the greater/greater-than comparison, we can combine mask and max.
					// Unfortunately there's no unsigned max on SSE2, it's synthesized by xoring 0x8000 on input and output.
					// We use AndNot to zero out Z results, before doing Max with the buffer.
					writeVal = shortZ.AndNot(shortMaskInv).Max(bufferValues);
					break;
				case ZCompareMode::Less:
					// This time, we OR the mask and use .Min.
					writeVal = (shortZ | shortMaskInv).Min(bufferValues);
					break;
				case ZCompareMode::Always:  // UNTESTED
					// This could be replaced with a vblend operation.
					writeVal = ((bufferValues & shortMaskInv) | shortZ.AndNot(shortMaskInv));
					break;
				}
				writeVal.Store(rowPtr + x);
				if (lowQ) {
					writeVal.Store(rowPtr + stride + x);
				}
			}
		}
	}
}

// This will always run on the main thread. Though, might consider moving the transforms out and just storing verts instead?
void DecodeAndTransformForDepthRaster(float *dest, const float *worldviewproj, const void *vertexData, int indexLowerBound, int indexUpperBound, const VertexDecoder *dec, u32 vertTypeID) {
	// TODO: Ditch skinned and morphed prims for now since we don't have a fast way to skin without running the full decoder.
	_dbg_assert_((vertTypeID & (GE_VTYPE_WEIGHT_MASK | GE_VTYPE_MORPHCOUNT_MASK)) == 0);

	int vertexStride = dec->VertexSize();
	int offset = dec->posoff;

	Mat4F32 mat(worldviewproj);

	const u8 *startPtr = (const u8 *)vertexData + indexLowerBound * vertexStride;
	int count = indexUpperBound - indexLowerBound + 1;

	switch (vertTypeID & GE_VTYPE_POS_MASK) {
	case GE_VTYPE_POS_FLOAT:
		for (int i = 0; i < count; i++) {
			const float *data = (const float *)(startPtr + i * vertexStride + offset);
			Vec4F32::Load(data).AsVec3ByMatrix44(mat).Store(dest + i * 4);
		}
		break;
	case GE_VTYPE_POS_16BIT:
		for (int i = 0; i < count; i++) {
			const s16 *data = ((const s16 *)((const s8 *)startPtr + i * vertexStride + offset));
			Vec4F32::LoadConvertS16(data).Mul(1.0f / 32768.f).AsVec3ByMatrix44(mat).Store(dest + i * 4);
		}
		break;
	case GE_VTYPE_POS_8BIT:
		for (int i = 0; i < count; i++) {
			const s8 *data = (const s8 *)startPtr + i * vertexStride + offset;
			Vec4F32::LoadConvertS8(data).Mul(1.0f / 128.0f).AsVec3ByMatrix44(mat).Store(dest + i * 4);
		}
		break;
	}
}

void TransformPredecodedForDepthRaster(float *dest, const float *worldviewproj, const void *decodedVertexData, const DecVtxFormat &fmt, int count) {
	int vertexStride = fmt.stride;
	int offset = fmt.posoff;

	Mat4F32 mat(worldviewproj);

	const u8 *startPtr = (const u8 *)decodedVertexData;
	// Decoded position format is always float3.
	for (int i = 0; i < count; i++) {
		const float *data = (const float *)(startPtr + i * vertexStride + offset);
		Vec4F32::Load(data).AsVec3ByMatrix44(mat).Store(dest + i * 4);
	}
}

void ConvertPredecodedThroughForDepthRaster(float *dest, const void *decodedVertexData, const VertexDecoder *dec, int count) {
	// TODO: Ditch skinned and morphed prims for now since we don't have a fast way to skin without running the full decoder.
	_dbg_assert_((dec->VertexType() & (GE_VTYPE_WEIGHT_MASK | GE_VTYPE_MORPHCOUNT_MASK)) == 0);

	int vertexStride = dec->GetDecVtxFmt().stride;
	int offset = dec->GetDecVtxFmt().posoff;

	const u8 *startPtr = (const u8 *)decodedVertexData;
	// Decoded position format is always float3.
	for (int i = 0; i < count; i++) {
		const float *data = (const float *)(startPtr + i * vertexStride + offset);
		// Just pass the position straight through - this is through mode!
		// A W of one makes projection a no-op, without branching.
		Vec4F32::Load(data).WithLane3One().Store(dest + i * 4);
	}
}

int DepthRasterClipIndexedRectangles(int *tx, int *ty, float *tz, const float *transformed, const uint16_t *indexBuffer, const DepthDraw &draw, const DepthScissor scissor) {
	int outCount = 0;
	const int count = draw.vertexCount;
	for (int i = 0; i < count; i += 2) {
		const float *verts[2] = {
			transformed + indexBuffer[i] * 4,
			transformed + indexBuffer[i + 1] * 4,
		};

		// Check if any vertex is behind the 0 plane.
		if (verts[0][3] < 0.0f || verts[1][3] < 0.0f) {
			// Ditch this rectangle.
			continue;
		}

		// These names are wrong .. until we transpose.
		// TODO: Maybe combine two rects here at a time. But hardly relevant for performance.
		Vec4F32 x = Vec4F32::Load(verts[0]);
		Vec4F32 y = Vec4F32::Load(verts[1]);
		Vec4F32 z = Vec4F32::Zero();
		Vec4F32 w = Vec4F32::Zero();
		Vec4F32::Transpose(x, y, z, w);
		// Now the names are accurate! Since we only have two vertices, the third and fourth member of each vector is zero
		// and will not be stored (well it will be stored, but it'll be overwritten by the next vertex).
		Vec4F32 recipW = w.Recip();

		x *= recipW;
		y *= recipW;
		z *= recipW;

		Vec4S32FromF32(x).Store2(tx + outCount);
		Vec4S32FromF32(y).Store2(ty + outCount);
		z.Clamp(0.0f, 65535.0f).Store2(tz + outCount);
		outCount += 2;
	}
	return outCount;
}

int DepthRasterClipIndexedTriangles(int *tx, int *ty, float *tz, const float *transformed, const uint16_t *indexBuffer, const DepthDraw &draw, const DepthScissor scissor, int maxOutCount, bool updateStats, const DepthScissor *columnCull) {
	int outCount = 0;
	int earlySize = 0;

	int flipCull = 0;
	if (draw.cullEnabled && draw.cullMode == GE_CULL_CW) {
		flipCull = 3;
	}

	static const float zerovec[4] = {0.0f, 0.0f, 0.0f, 1.0f};

	int collected = 0;
	int planeCulled = 0;
	int boxCulled = 0;
	const float *verts[12];  // four triangles at a time!
	const int count = draw.vertexCount;

	// Not exactly the same guardband as on the real PSP, but good enough to prevent 16-bit overflow in raster.
	// This is slightly off-center since we are already in screen space, but whatever.
	Vec4S32 guardBandTopLeft = Vec4S32::Splat(-4096);
	Vec4S32 guardBandBottomRight = Vec4S32::Splat(4096);

	Vec4S32 scissorX1 = Vec4S32::Splat((float)scissor.x1);
	Vec4S32 scissorY1 = Vec4S32::Splat((float)scissor.y1);
	Vec4S32 scissorX2 = Vec4S32::Splat((float)scissor.x2);
	Vec4S32 scissorY2 = Vec4S32::Splat((float)scissor.y2);
	const Vec4S32 columnX1 = Vec4S32::Splat(columnCull ? (int)columnCull->x1 : 0);
	const Vec4S32 columnX2 = Vec4S32::Splat(columnCull ? (int)columnCull->x2 : 0);

	// Add cheap pre-projection pre-checks for bad triangle here. Not much we can do safely other than checking W.
	auto validVert = [](const float *v) -> bool {
		if (v[3] <= 0.0f || v[2] <= 0.0f) {
			return false;
		}
		/*
		if (v[2] >= 65535.0f * v[3]) {
			return false;
		}*/
		return true;
	};

	const bool cullDisabled = !draw.cullEnabled;

	for (int i = 0; i < count; i += 3) {
		// Collect valid triangles into buffer.
		const float *v0 = transformed + indexBuffer[i] * 4;
		const float *v1 = transformed + indexBuffer[i + (1 ^ flipCull)] * 4;
		const float *v2 = transformed + indexBuffer[i + (2 ^ flipCull)] * 4;
		// Don't collect triangle if any vertex is beyond the planes.
		// TODO: Optimize this somehow.
		if (validVert(v0) && validVert(v1) && validVert(v2)) {
			verts[collected] = v0;
			verts[collected + 1] = v1;
			verts[collected + 2] = v2;
			if (cullDisabled) {
				collected += 3;
				// Add the reverse triangle too. We could alternatively handle culling later in the pipeline, but mainly
				// Syphon Filter needs this (issue #21498) and this simplifies things.
				verts[collected] = v0;
				verts[collected + 1] = v2;
				verts[collected + 2] = v1;
			}
			collected += 3;
		} else {
			planeCulled++;
		}

		if (i >= count - 3 && collected != 12) {
			// Last iteration. Zero out any remaining triangles.
			for (int j = collected; j < 12; j++) {
				verts[j] = zerovec;
			}
			collected = 12;
		}

		if (collected < 12) {
			// Fetch more!
			continue;
		}

		collected -= 12;

		if (outCount + 12 > maxOutCount) {
			// Out of room. With culling off we emit two triangles per input triangle, so this is reachable.
			break;
		}

		// These names are wrong .. until we transpose.
		Vec4F32 x0 = Vec4F32::Load(verts[0]);
		Vec4F32 x1 = Vec4F32::Load(verts[1]);
		Vec4F32 x2 = Vec4F32::Load(verts[2]);
		Vec4F32 y0 = Vec4F32::Load(verts[3]);
		Vec4F32 y1 = Vec4F32::Load(verts[4]);
		Vec4F32 y2 = Vec4F32::Load(verts[5]);
		Vec4F32 z0 = Vec4F32::Load(verts[6]);
		Vec4F32 z1 = Vec4F32::Load(verts[7]);
		Vec4F32 z2 = Vec4F32::Load(verts[8]);
		Vec4F32 w0 = Vec4F32::Load(verts[9]);
		Vec4F32 w1 = Vec4F32::Load(verts[10]);
		Vec4F32 w2 = Vec4F32::Load(verts[11]);

		Vec4F32::Transpose(x0, y0, z0, w0);
		Vec4F32::Transpose(x1, y1, z1, w1);
		Vec4F32::Transpose(x2, y2, z2, w2);

		// Now the names are accurate!

		// Let's project all three vertices, for all four triangles.
		Vec4F32 recipW0 = w0.Recip();
		Vec4F32 recipW1 = w1.Recip();
		Vec4F32 recipW2 = w2.Recip();
		x0 *= recipW0;
		y0 *= recipW0;
		z0 *= recipW0;
		x1 *= recipW1;
		y1 *= recipW1;
		z1 *= recipW1;
		x2 *= recipW2;
		y2 *= recipW2;
		z2 *= recipW2;

		// Check bounding box size. Cast to integer for crude rounding (and to approximately match the rasterizer).
		Vec4S32 minX = Vec4S32FromF32(x0.Min(x1.Min(x2)));
		Vec4S32 minY = Vec4S32FromF32(y0.Min(y1.Min(y2)));
		Vec4S32 maxX = Vec4S32FromF32(x0.Max(x1.Max(x2)));
		Vec4S32 maxY = Vec4S32FromF32(y0.Max(y1.Max(y2)));

		// If all are equal in any dimension, all four triangles are tiny nonsense and can be skipped early.
		Vec4S32 eqMask = minX.CompareEq(maxX) | minY.CompareEq(maxY);

		// Otherwise we just proceed to triangle setup with all four for now.
		// We could also save the computed boxes for later..
		// TODO: Merge into below checks? Though nice with an early out.
		if (!AnyZeroSignBit(eqMask)) {
			boxCulled += 4;
			continue;
		}

		// Create a mask to kill coordinates of triangles that poke outside the guardband (or are just empty).
		Vec4S32 inGuardBand =
			((minX.CompareGt(guardBandTopLeft) & maxX.CompareLt(guardBandBottomRight)) &
				(minY.CompareGt(guardBandTopLeft) & maxY.CompareLt(guardBandBottomRight))).AndNot(eqMask);

		// Create another mask to kill off-screen triangles. Not perfectly accurate.
		inGuardBand &= (maxX.CompareGt(scissorX1) & minX.CompareLt(scissorX2)) & (maxY.CompareGt(scissorY1) & minY.CompareLt(scissorY2));

		// It's enough to smash one coordinate to make future checks (like the tri area check) fail.
		x0 &= inGuardBand;
		x1 &= inGuardBand;
		x2 &= inGuardBand;

		// Floating point double triangle area. Can't be reused for the integer-snapped raster reliably (though may work...)
		// Still good for backface culling early and pretty cheap to compute.
		Vec4F32 doubleTriArea = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0) - Vec4F32::Splat((float)(MIN_TWICE_TRI_AREA));
		if (!AnyZeroSignBit(doubleTriArea)) {
			earlySize += 4;
			continue;
		}

		if (columnCull) {
			// Drop the triangles that can't reach the columns. Not before the checks above: those look at all four
			// triangles together, so they have to see the same ones as with the whole scissor.
			const Vec4S32 inColumns = maxX.CompareGt(columnX1) & minX.CompareLt(columnX2);
			if (!AnyZeroSignBit(Vec4S32::Splat(-1).AndNot(inColumns))) {
				continue;  // None of the four.
			}
			x0 &= inColumns;
			x1 &= inColumns;
			x2 &= inColumns;
		}

		// Note: If any triangle is outside the guardband, (just) its X coords get zeroed, and it'll later get rejected.
		Vec4S32FromF32(x0).Store(tx + outCount);
		Vec4S32FromF32(x1).Store(tx + outCount + 4);
		Vec4S32FromF32(x2).Store(tx + outCount + 8);
		Vec4S32FromF32(y0).Store(ty + outCount);
		Vec4S32FromF32(y1).Store(ty + outCount + 4);
		Vec4S32FromF32(y2).Store(ty + outCount + 8);
		z0.Store(tz + outCount);
		z1.Store(tz + outCount + 4);
		z2.Store(tz + outCount + 8);

#ifdef _DEBUG
		for (int i = 0; i < 12; i++) {
			_dbg_assert_(tx[outCount + i] < 32767);
			_dbg_assert_(tx[outCount + i] >= -32768);
			_dbg_assert_(tx[outCount + i] < 32767);
			_dbg_assert_(tx[outCount + i] >= -32768);
		}
#endif

		outCount += 12;
	}

	if (updateStats) {
		gpuStats.perFrame.numDepthRasterZCulled += planeCulled;
		gpuStats.perFrame.numDepthEarlyBoxCulled += boxCulled;
		gpuStats.perFrame.numDepthRasterEarlySize += earlySize;
	}
	return outCount;
}

int DepthRasterClip(int *tx, int *ty, float *tz, const float *transformed, const uint16_t *indexBuffer, const DepthDraw &draw, const DepthScissor columns, int maxOutCount, bool updateStats) {
	if (draw.prim == GE_PRIM_RECTANGLES) {
		return DepthRasterClipIndexedRectangles(tx, ty, tz, transformed, indexBuffer, draw, columns);
	}
	if (draw.prim != GE_PRIM_TRIANGLES) {
		_dbg_assert_(false);
		return 0;
	}
	// The clipper only roughly culls (strict compares on rounded bounds). With a margin around the columns, it
	// keeps everything that can reach them, and the rasterizer clamps exactly.
	DepthScissor cull = columns;
	cull.x1 = (u16)std::max((int)draw.scissor.x1, (int)columns.x1 - 8);
	cull.x2 = (u16)std::min((int)draw.scissor.x2, (int)columns.x2 + 8);
	const bool narrower = cull.x1 != draw.scissor.x1 || cull.x2 != draw.scissor.x2;
	return DepthRasterClipIndexedTriangles(tx, ty, tz, transformed, indexBuffer, draw, draw.scissor, maxOutCount, updateStats, narrower ? &cull : nullptr);
}

// Rasterizes screen-space vertices.
void DepthRasterScreenVerts(uint16_t *depth, int depthStride, const int *tx, const int *ty, const float *tz, int count, const DepthDraw &draw, const DepthScissor scissor, bool lowQ, bool updateStats, bool skipEmptyBlocks, const DepthScissor *columns) {
	// Prim should now be either TRIANGLES or RECTs.
	_dbg_assert_(draw.prim == GE_PRIM_RECTANGLES || draw.prim == GE_PRIM_TRIANGLES);
	// Triangles are culled against the whole scissor, so the columns only limit which groups get written.
	const DepthScissor written = columns ? *columns : scissor;
	const int tileX1 = written.x1 & ~3;
	const int tileX2 = written.x2 & ~3;

	switch (draw.prim) {
	case GE_PRIM_RECTANGLES:
		for (int i = 0; i < count; i += 2) {
			uint16_t z = (uint16_t)tz[i + 1];  // depth from second vertex
			// TODO: Should clip coordinates to the scissor rectangle.
			// We remove the subpixel information here.
			DepthRasterRect(depth, depthStride, written, tx[i], ty[i], tx[i + 1], ty[i + 1], z, draw.compareMode);
		}
		if (updateStats) {
			gpuStats.perFrame.numDepthRasterPrims += count / 2;
		}
		break;
	case GE_PRIM_TRIANGLES:
	{
		int stats[3]{};
		// Batches of 4 triangles, as output by the clip function.
		if (lowQ) {
			switch (draw.compareMode) {
			case ZCompareMode::Greater:
			{
				for (int i = 0; i < count; i += 12) {
					DepthRaster4Triangles<ZCompareMode::Greater, true>(stats, depth, depthStride, scissor, tileX1, tileX2, &tx[i], &ty[i], &tz[i], skipEmptyBlocks);
				}
				break;
			}
			case ZCompareMode::Less:
			{
				for (int i = 0; i < count; i += 12) {
					DepthRaster4Triangles<ZCompareMode::Less, true>(stats, depth, depthStride, scissor, tileX1, tileX2, &tx[i], &ty[i], &tz[i], skipEmptyBlocks);
				}
				break;
			}
			case ZCompareMode::Always:
			{
				for (int i = 0; i < count; i += 12) {
					DepthRaster4Triangles<ZCompareMode::Always, true>(stats, depth, depthStride, scissor, tileX1, tileX2, &tx[i], &ty[i], &tz[i], skipEmptyBlocks);
				}
				break;
			}
			}
		} else {
			switch (draw.compareMode) {
			case ZCompareMode::Greater:
			{
				for (int i = 0; i < count; i += 12) {
					DepthRaster4Triangles<ZCompareMode::Greater, false>(stats, depth, depthStride, scissor, tileX1, tileX2, &tx[i], &ty[i], &tz[i], skipEmptyBlocks);
				}
				break;
			}
			case ZCompareMode::Less:
			{
				for (int i = 0; i < count; i += 12) {
					DepthRaster4Triangles<ZCompareMode::Less, false>(stats, depth, depthStride, scissor, tileX1, tileX2, &tx[i], &ty[i], &tz[i], skipEmptyBlocks);
				}
				break;
			}
			case ZCompareMode::Always:
			{
				for (int i = 0; i < count; i += 12) {
					DepthRaster4Triangles<ZCompareMode::Always, false>(stats, depth, depthStride, scissor, tileX1, tileX2, &tx[i], &ty[i], &tz[i], skipEmptyBlocks);
				}
				break;
			}
			}
		}
		if (updateStats) {
			gpuStats.perFrame.numDepthRasterNoPixels += stats[(int)TriangleStat::NoPixels];
			gpuStats.perFrame.numDepthRasterTooSmall += stats[(int)TriangleStat::SmallOrBackface];
			gpuStats.perFrame.numDepthRasterPrims += stats[(int)TriangleStat::OK];
		}
		break;
	}
	default:
		_dbg_assert_(false);
	}
}

// The depth buffers are all in VRAM, so busy ranges are kept as offsets into it, which also covers
// the mirrors.
static constexpr u32 VRAM_OFFSET_MASK = 0x001FFFFF;

// A scissor reaching past the stride writes into the next rows, which are other tiles' columns there.
// So the first tile does the whole draw, with the others waiting before and after it.
static bool DepthDrawWraps(const DepthDraw &draw) {
	return draw.scissor.x2 >= draw.depthStride;
}

// Which tile owns a pixel depends on the buffer's address and stride.
static u64 DepthBufferKey(const DepthDraw &draw) {
	return ((u64)draw.depthAddr << 16) | draw.depthStride | ((u64)DepthDrawWraps(draw) << 63);
}

// The width that tiles split: the screen's, or the stride if that's narrower.
static int DepthTileWidth(const DepthDraw &draw) {
	return std::min((int)draw.depthStride, 480);
}

DepthRasterWorker::DepthRasterWorker(int maxVerts, int maxIndices, int numTiles) : maxVerts_(maxVerts), maxIndices_(maxIndices), tiles_(std::max(numTiles, 1)) {
	for (TileThread &tile : tiles_) {
		// x, y and z, each with room for a group of four triangles past the end.
		tile.screenVerts = (int *)AllocateMemoryPages((maxVerts_ * sizeof(int) + 384) * 3, MEM_PROT_READ | MEM_PROT_WRITE);
	}
	current_ = NewBatch();
	for (int i = 0; i < (int)tiles_.size(); ++i) {
		tiles_[i].thread = std::thread([this, i] { Run(i); });
	}
}

DepthRasterWorker::~DepthRasterWorker() {
	Wait();
	{
		std::lock_guard<std::mutex> guard(mutex_);
		quit_ = true;
	}
	work_.notify_all();
	for (TileThread &tile : tiles_) {
		tile.thread.join();
	}
	for (DepthBatch *batch : all_) {
		FreeBatch(batch);
	}
	for (TileThread &tile : tiles_) {
		FreeMemoryPages(tile.screenVerts, (maxVerts_ * sizeof(int) + 384) * 3);
	}
}

DepthBatch *DepthRasterWorker::NewBatch() {
	DepthBatch *batch = new DepthBatch();
	batch->draws.reserve(16);
	all_.push_back(batch);
	return batch;
}

void DepthRasterWorker::FreeBatch(DepthBatch *batch) {
	FreeAlignedMemory(batch->transformed);
	FreeAlignedMemory(batch->indices);
	delete batch;
}

void DepthRasterWorker::Reserve(int vertices, int indices) {
	DepthBatch &batch = *current_;
	const int needVerts = batch.vertexCount + vertices;
	if (needVerts > batch.vertexCapacity) {
		const int capacity = std::min(std::max(needVerts, batch.vertexCapacity * 2), maxVerts_);
		float *grown = (float *)AllocateAlignedMemory(capacity * 4 * sizeof(float), 16);
		if (batch.transformed) {
			memcpy(grown, batch.transformed, batch.vertexCount * 4 * sizeof(float));
			FreeAlignedMemory(batch.transformed);
		}
		batch.transformed = grown;
		batch.vertexCapacity = capacity;
	}
	const int needIndices = batch.indexCount + indices;
	if (needIndices > batch.indexCapacity) {
		const int capacity = std::min(std::max(needIndices, batch.indexCapacity * 2), maxIndices_);
		uint16_t *grown = (uint16_t *)AllocateAlignedMemory(capacity * sizeof(uint16_t), 16);
		if (batch.indices) {
			memcpy(grown, batch.indices, batch.indexCount * sizeof(uint16_t));
			FreeAlignedMemory(batch.indices);
		}
		batch.indices = grown;
		batch.indexCapacity = capacity;
	}
}

void DepthRasterWorker::SubmitIfReady() {
	if (current_->draws.size() >= 8 || current_->vertexCount >= 1024) {
		Submit();
	}
}

void DepthRasterWorker::Submit() {
	if (current_->draws.empty()) {
		return;
	}
	// Wipeout submits about 40 a frame. Past this many, wait for one to come back.
	constexpr size_t MAX_BATCHES = 128;
	std::unique_lock<std::mutex> lock(mutex_);
	for (const DepthDraw &draw : current_->draws) {
		const u32 low = (draw.depthAddr & VRAM_OFFSET_MASK) + draw.scissor.y1 * draw.depthStride * 2;
		const u32 high = (draw.depthAddr & VRAM_OFFSET_MASK) + (draw.scissor.y2 + 1) * draw.depthStride * 2;
		if (!busy_) {
			busyRanges_.clear();
			busyLow_ = low;
			busyHigh_ = high;
			busy_ = true;
		} else {
			busyLow_ = std::min((u32)busyLow_, low);
			busyHigh_ = std::max((u32)busyHigh_, high);
		}
		bool merged = false;
		for (auto &range : busyRanges_) {
			if (low <= range.second && high >= range.first) {
				range.first = std::min(range.first, low);
				range.second = std::max(range.second, high);
				merged = true;
				break;
			}
		}
		if (!merged) {
			busyRanges_.emplace_back(low, high);
		}
	}
	if (queue_.empty() && tiles_.size() == 2) {
		Rebalance();
	}
	current_->split = split_;
	current_->pendingTiles = (int)tiles_.size();
	current_->firstDraw = submittedDraws_;
	current_->previousBuffer = lastBuffer_;
	submittedDraws_ += current_->draws.size();
	lastBuffer_ = DepthBufferKey(current_->draws.back());
	queue_.push_back(current_);
	work_.notify_all();
	if (free_.empty() && all_.size() < MAX_BATCHES) {
		lock.unlock();
		current_ = NewBatch();
		return;
	}
	done_.wait(lock, [this] { return !free_.empty(); });
	current_ = free_.back();
	free_.pop_back();
	current_->draws.clear();
	current_->vertexCount = 0;
	current_->indexCount = 0;
}

void DepthRasterWorker::Wait() {
	Submit();
	WaitForSubmitted();
}

void DepthRasterWorker::WaitForSubmitted() {
	if (!busy_) {
		return;
	}
	std::unique_lock<std::mutex> lock(mutex_);
	done_.wait(lock, [this] { return queue_.empty(); });
}

void DepthRasterWorker::WaitIfWriting(u32 addr, u32 size) {
	// The mask below would map a RAM address onto the VRAM offsets too. Midnight Club L.A. Remix loads CLUTs
	// from RAM addresses that land on its depth buffer that way, and waited for the worker on each.
	if (!busy_ || !Memory::IsVRAMAddress(addr)) {
		return;
	}
	const u32 low = addr & VRAM_OFFSET_MASK;
	if (low >= busyHigh_ || low + size <= busyLow_) {
		return;
	}
	std::unique_lock<std::mutex> lock(mutex_);
	bool overlaps = false;
	for (const auto &range : busyRanges_) {
		overlaps = overlaps || (low < range.second && low + size > range.first);
	}
	if (busy_ && overlaps) {
		done_.wait(lock, [this] { return queue_.empty(); });
	}
}

void DepthRasterWorker::TakeTimes(double *cull, double *raster) {
	std::lock_guard<std::mutex> guard(mutex_);
	*cull += cullTime_;
	*raster += rasterTime_;
	cullTime_ = 0.0;
	rasterTime_ = 0.0;
}

// Moves the boundary between two tiles so they take about as long. Only called with the queue empty, so no
// tile is using the columns it changes. A busy render core slows the second tile, which then gets less.
void DepthRasterWorker::Rebalance() {
	const double first = tiles_[0].busy;
	const double second = tiles_[1].busy;
	if (first + second < 0.004) {
		return;  // Too little to go on.
	}
	// Time per column on each side, as if spread evenly over it. Equal times leave the split where it is.
	const double split = (double)split_ / DEPTH_SPLIT_ONE;
	const double firstDensity = first / split;
	const double secondDensity = second / (1.0 - split);
	const double balanced = secondDensity / (firstDensity + secondDensity);
	split_ = std::clamp((int)(DEPTH_SPLIT_ONE * (split + balanced) * 0.5), DEPTH_SPLIT_ONE / 8, DEPTH_SPLIT_ONE * 7 / 8);
	tiles_[0].busy = 0.0;
	tiles_[1].busy = 0.0;
}

void DepthRasterWorker::Run(int tile) {
	// Further tiles go to the render core, at a priority that leaves it to a render thread first.
	SetCurrentThreadAffinity(tile == 0 ? ThreadAffinityRole::COMPUTE : ThreadAffinityRole::RENDER_CORE_COMPUTE);
	SetCurrentThreadName(tile == 0 ? "DepthRaster" : "DepthRasterTile");
	TileThread &self = tiles_[tile];
	std::unique_lock<std::mutex> lock(mutex_);
	while (true) {
		work_.wait(lock, [&] { return quit_ || self.next < queueStart_ + queue_.size(); });
		if (quit_) {
			break;
		}
		DepthBatch *batch = queue_[self.next - queueStart_];
		lock.unlock();

		double cull = 0.0, raster = 0.0;
		Rasterize(*batch, tile, &cull, &raster);

		lock.lock();
		self.next++;
		self.busy += cull + raster;
		cullTime_ += cull;
		rasterTime_ += raster;
		if (--batch->pendingTiles == 0) {
			// Every tile goes through the queue in order, so the last to finish a batch finishes the oldest.
			_dbg_assert_(batch == queue_.front());
			queue_.pop_front();
			queueStart_++;
			free_.push_back(batch);
			if (queue_.empty()) {
				busy_ = false;
			}
			done_.notify_all();
		}
	}
}

void DepthRasterWorker::Rasterize(const DepthBatch &batch, int tile, double *cull, double *raster) {
	const int numTiles = (int)tiles_.size();
	int *tx = tiles_[tile].screenVerts;
	int *ty = tx + maxVerts_;
	float *tz = (float *)(tx + maxVerts_ * 2);
	u64 previousBuffer = batch.previousBuffer;
	for (size_t i = 0; i < batch.draws.size(); ++i) {
		const DepthDraw &draw = batch.draws[i];
		if (numTiles > 1) {
			if (i != 0) {
				FinishDraw(tile, batch.firstDraw + i);
			}
			// Tiles own the same columns as long as the buffer stays the same. Before one changes, all earlier
			// draws must be done, since the columns can be elsewhere in memory now.
			const u64 buffer = DepthBufferKey(draw);
			if (buffer != previousBuffer) {
				WaitForTiles(batch.firstDraw + i);
				previousBuffer = buffer;
			}
		}
		const float *vertices = batch.transformed + 4 * draw.vertexOffset;
		const uint16_t *indices = batch.indices + draw.indexOffset;
		const bool whole = numTiles == 1 || DepthDrawWraps(draw);
		const DepthScissor columns = whole ? draw.scissor : draw.scissor.Tile(tile, numTiles, DepthTileWidth(draw), batch.split);
		if (tile != 0 && (whole || columns.x1 > columns.x2)) {
			continue;  // The scissor misses this tile's columns.
		}

		double start = time_now_d();
		// With several tiles, the first counts the stats (for its columns).
		const int outVertCount = DepthRasterClip(tx, ty, tz, vertices, indices, draw, columns, maxVerts_, tile == 0);
		double mid = time_now_d();
		*cull += mid - start;
		if (outVertCount > 0 && Memory::IsValid4AlignedAddress(draw.depthAddr)) {
			u16 *depthPtr = (uint16_t *)Memory::GetPointerWriteUnchecked(draw.depthAddr);
			DepthRasterScreenVerts(depthPtr, draw.depthStride, tx, ty, tz, outVertCount, draw, draw.scissor, batch.lowQ, tile == 0, true, &columns);
			*raster += time_now_d() - mid;
		}
	}
	if (numTiles > 1 && !batch.draws.empty()) {
		FinishDraw(tile, batch.firstDraw + batch.draws.size());
	}
}

void DepthRasterWorker::WaitForTiles(u64 draw) {
	std::unique_lock<std::mutex> lock(mutex_);
	auto caughtUp = [&] {
		for (const TileThread &tile : tiles_) {
			if (tile.drawsDone < draw) {
				return false;
			}
		}
		return true;
	};
	if (!caughtUp()) {
		tileProgress_.wait(lock, caughtUp);
	}
}

void DepthRasterWorker::FinishDraw(int tile, u64 draws) {
	{
		std::lock_guard<std::mutex> guard(mutex_);
		tiles_[tile].drawsDone = draws;
	}
	tileProgress_.notify_all();
}
