#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "Common/CommonTypes.h"
#include "GPU/ge_constants.h"

struct DepthScreenVertex {
	int x;
	int y;
	int z;
};

// X11.h, sigh.
#ifdef Always
#undef Always
#endif

// We only need to support these three modes.
enum class ZCompareMode : u8 {
	Greater,  // Most common
	Less,  // Less common
	Always,  // Mostly used for clears
};

constexpr int DEPTH_SPLIT_ONE = 1024;

struct DepthScissor {
	u16 x1;
	u16 y1;
	u16 x2;
	u16 y2;

	// The part of the scissor in one of numTiles column ranges of a buffer. With two tiles, split is where
	// the first ends, in DEPTH_SPLIT_ONE units of the width.
	DepthScissor Tile(int tile, int numTiles, int bufferWidth, int split = DEPTH_SPLIT_ONE / 2) const;
};

struct DepthDraw {
	u32 depthAddr;
	u16 depthStride;
	u8 cullMode;
	GEPrimitiveType prim;
	ZCompareMode compareMode;
	bool cullEnabled;
	DepthScissor scissor;
	int vertexOffset;
	int indexOffset;
	int vertexCount;
};

// Specialized, very limited depth-only rasterizer.
// Meant to run in parallel with hardware rendering, in games that read back the depth buffer
// for effects like lens flare.
// So, we can be quite inaccurate without any issues, and skip a lot of functionality.

class VertexDecoder;
struct TransformedVertex;

// maxOutCount is the capacity of each of tx/ty/tz, in elements. With columnCull, triangles outside its X range
// are dropped too.
int DepthRasterClipIndexedTriangles(int *tx, int *ty, float *tz, const float *transformed, const uint16_t *indexBuffer, const DepthDraw &draw, const DepthScissor scissor, int maxOutCount, bool updateStats = true, const DepthScissor *columnCull = nullptr);
int DepthRasterClipIndexedRectangles(int *tx, int *ty, float *tz, const float *transformed, const uint16_t *indexBuffer, const DepthDraw &draw, const DepthScissor scissor);
// Projects and culls a draw for rasterizing the columns of its scissor (from DepthScissor::Tile, or all of it).
int DepthRasterClip(int *tx, int *ty, float *tz, const float *transformed, const uint16_t *indexBuffer, const DepthDraw &draw, const DepthScissor columns, int maxOutCount, bool updateStats);
void DecodeAndTransformForDepthRaster(float *dest, const float *worldviewproj, const void *vertexData, int indexLowerBound, int indexUpperBound, const VertexDecoder *dec, u32 vertTypeID);
void TransformPredecodedForDepthRaster(float *dest, const float *worldviewproj, const void *decodedVertexData, const VertexDecoder *dec, int count);
void ConvertPredecodedThroughForDepthRaster(float *dest, const void *decodedVertexData, const VertexDecoder *dec, int count);
// With columns (from scissor.Tile()), only those are written, with the same result as rasterizing the whole scissor.
void DepthRasterScreenVerts(uint16_t *depth, int depthStride, const int *tx, const int *ty, const float *tz, int count, const DepthDraw &draw, const DepthScissor scissor, bool lowQ, bool updateStats = true, bool skipEmptyBlocks = true, const DepthScissor *columns = nullptr);

// A batch of queued depth draws, with their transformed vertices and indices.
struct DepthBatch {
	float *transformed = nullptr;
	uint16_t *indices = nullptr;
	std::vector<DepthDraw> draws;
	int vertexCount = 0;
	int indexCount = 0;
	int vertexCapacity = 0;
	int indexCapacity = 0;
	bool lowQ = false;
	int pendingTiles = 0;  // Tiles that haven't finished this batch yet.
	u64 firstDraw = 0;  // Sequence number of the first draw.
	u64 previousBuffer = 0;  // DepthBufferKey of the draw before it.
	int split = DEPTH_SPLIT_ONE / 2;  // Between two tiles, see DepthScissor::Tile.
};

// Rasterizes batches of depth draws on threads of its own, in the order they're submitted, so
// that it overlaps with emulating the CPU and the rest of the GE. The PSP memory it writes is only
// the depth buffers of the draws, so anything reading those has to Wait() first (or WaitIfWriting).
class DepthRasterWorker {
public:
	// Each of numTiles threads rasterizes one column range (DepthScissor::Tile) of every batch.
	DepthRasterWorker(int maxVerts, int maxIndices, int numTiles = 1);
	~DepthRasterWorker();

	// The batch to add draws to.
	DepthBatch &Current() { return *current_; }
	// Makes room in the current batch for this many more vertices and indices.
	void Reserve(int vertices, int indices);
	// Starts rasterizing the current batch (if it has draws) and makes an empty one current.
	void Submit();
	// Starts a bounded batch early so rasterization overlaps with producing later draws.
	void SubmitIfReady();
	// Submits the current batch and waits until all are rasterized.
	void Wait();
	// Waits until the batches already submitted are rasterized.
	void WaitForSubmitted();
	// Waits only if a batch being rasterized may write to [addr, addr + size).
	void WaitIfWriting(u32 addr, u32 size);

	// Time spent culling and rasterizing since the last call, for the stats.
	void TakeTimes(double *cull, double *raster);

private:
	DepthBatch *NewBatch();
	static void FreeBatch(DepthBatch *batch);
	void Run(int tile);
	void Rasterize(const DepthBatch &batch, int tile, double *cull, double *raster);

	void WaitForTiles(u64 draw);
	void FinishDraw(int tile, u64 draws);
	void Rebalance();

	struct TileThread {
		std::thread thread;
		int *screenVerts = nullptr;
		u64 next = 0;  // Sequence number of the next batch to rasterize.
		u64 drawsDone = 0;
		double busy = 0.0;  // Seconds spent rasterizing since the last Rebalance().
	};

	const int maxVerts_;
	const int maxIndices_;
	std::vector<TileThread> tiles_;

	std::vector<DepthBatch *> all_;
	std::vector<DepthBatch *> free_;
	std::deque<DepthBatch *> queue_;  // Submitted and not finished by every tile, oldest first.
	u64 queueStart_ = 0;  // Sequence number of queue_.front().
	u64 submittedDraws_ = 0;
	u64 lastBuffer_ = 0;
	int split_ = DEPTH_SPLIT_ONE / 2;
	DepthBatch *current_ = nullptr;

	// The depth buffer bytes the submitted, unfinished batches can write.
	// The depth buffers being written (VRAM offsets, end exclusive), and their span, which only grows
	// while busy_, so it can be checked without the lock.
	std::vector<std::pair<u32, u32>> busyRanges_;
	std::atomic<u32> busyLow_{};
	std::atomic<u32> busyHigh_{};
	std::atomic<bool> busy_{};

	double cullTime_ = 0.0;
	double rasterTime_ = 0.0;

	bool quit_ = false;
	std::mutex mutex_;
	std::condition_variable work_;
	std::condition_variable done_;
	std::condition_variable tileProgress_;
};
