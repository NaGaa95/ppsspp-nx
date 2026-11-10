// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <future>
#include <thread>

#include "Common/Thread/ThreadUtil.h"
#include "GPU/Common/DepthRaster.h"

using namespace std::chrono_literals;
#define _dbg_assert_(value) assert(value)

// The worker's allocation, affinity and VRAM classification dependencies are controlled here.
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
namespace Memory {
bool IsVRAMAddress(u32 address) { return (address & 0x3F000000) == 0x04000000; }
}

struct RasterGate {
	std::mutex mutex;
	std::condition_variable cv;
	bool enabled = false;
	bool entered = false;
	bool released = false;
	void Reset(bool block) {
		std::lock_guard<std::mutex> lock(mutex);
		enabled = block;
		entered = released = false;
	}
	void Enter() {
		std::unique_lock<std::mutex> lock(mutex);
		if (enabled && !entered) {
			entered = true;
			cv.notify_all();
			cv.wait(lock, [&] { return released; });
		}
	}
	void Await() {
		std::unique_lock<std::mutex> lock(mutex);
		assert(cv.wait_for(lock, 5s, [&] { return entered; }));
	}
	void Release() {
		std::lock_guard<std::mutex> lock(mutex);
		released = true;
		cv.notify_all();
	}
} gate;

// One result per tile thread: each goes through every batch, in order.
int tiles = 1;
std::array<std::array<u16, 128>, 2> outputs{};
std::array<int, 2> processed{};
void Reset() {
	outputs = {};
	processed = {};
}
void Apply(std::array<u16, 128> &buffer, int value) {
	const int x = value % buffer.size();
	buffer[x] = (u16)(buffer[x] * 33 + value);
}

// This callback deliberately overlaps writes between draws: FIFO ordering affects the result.
// Pixel coverage/interpolation are checked separately by TestDepthRaster and its sanitizer fixture.
void DepthRasterWorker::Rasterize(const DepthBatch &batch, int tile, double *, double *) {
	assert(tile >= 0 && tile < tiles);
	gate.Enter();
	for (const auto &draw : batch.draws) {
		const int value = (int)batch.transformed[draw.vertexOffset * 4];
		assert(batch.indices[draw.indexOffset] == value);
		Apply(outputs[tile], value);
		++processed[tile];
	}
}

// PRODUCTION_WORKER

void Append(DepthRasterWorker &worker, int value, bool early, int vertices = 1) {
	worker.Reserve(vertices, 1);
	auto &batch = worker.Current();
	std::fill(batch.transformed + batch.vertexCount * 4, batch.transformed + (batch.vertexCount + vertices) * 4, 0.0f);
	batch.transformed[batch.vertexCount * 4] = (float)value;
	batch.indices[batch.indexCount] = value;
	DepthDraw draw{};
	draw.depthAddr = 0x04000000;
	draw.depthStride = 128;
	draw.scissor = {0, 0, 127, 0};
	draw.vertexOffset = batch.vertexCount;
	draw.indexOffset = batch.indexCount;
	draw.vertexCount = vertices;
	batch.vertexCount += vertices;
	++batch.indexCount;
	batch.draws.push_back(draw);
	if (early) {
		worker.SubmitIfReady();
	}
}

void Check(int count) {
	std::array<u16, 128> reference{};
	for (int i = 1; i <= count; ++i) {
		Apply(reference, i);
	}
	for (int tile = 0; tile < tiles; ++tile) {
		assert(outputs[tile] == reference);
		assert(processed[tile] == count);
	}
}

void Ordered(bool early) {
	Reset();
	gate.Reset(early);
	DepthRasterWorker worker(2048, 2048, tiles);
	for (int i = 1; i <= 37; ++i) {
		Append(worker, i, early);
		if (early && i == 8) {
			assert(worker.Current().draws.empty());
			gate.Await();  // The worker has started before the producer finishes its list.
			worker.WaitIfWriting(0x08000000, 256);
			worker.WaitIfWriting(0x04001000, 256);
			auto waiter = std::async(std::launch::async, [&] { worker.WaitIfWriting(0x04000000, 2); });
			assert(waiter.wait_for(20ms) == std::future_status::timeout);
			gate.Release();
			waiter.get();
		}
	}
	worker.Wait();
	Check(37);
}

void Backpressure() {
	Reset();
	gate.Reset(true);
	DepthRasterWorker worker(2048, 2048, tiles);
	std::atomic<int> committed{0};
	auto producer = std::async(std::launch::async, [&] {
		for (int i = 1; i <= 1100; ++i) {
			Append(worker, i, true);
			committed = i;
		}
	});
	gate.Await();
	const auto deadline = std::chrono::steady_clock::now() + 5s;
	while (committed < 1000 && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::yield();
	}
	assert(committed >= 1000);
	assert(producer.wait_for(20ms) == std::future_status::timeout);
	gate.Release();
	producer.get();
	worker.Wait();
	Check(1100);
}

void Scenarios() {
	Ordered(false);
	Ordered(true);
	Backpressure();
	Reset();
	gate.Reset(false);
	{
		DepthRasterWorker worker(2048, 2048, tiles);
		for (int i = 1; i <= 9; ++i) {
			Append(worker, i, true);
		}
	}  // Queued work and the partial batch must complete before their storage is freed.
	Check(9);
	Reset();
	{
		DepthRasterWorker worker(2048, 2048, tiles);
		Append(worker, 1, true, 1024);
		assert(worker.Current().draws.empty());
		worker.Wait();
		Check(1);
	}
}

int main(int argc, char **argv) {
	assert(argc == 2);
	for (tiles = 1; tiles <= 2; ++tiles) {
		Scenarios();
	}
}
