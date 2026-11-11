// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.

#include <cassert>
#include <cstdio>
#include <random>

#include "Common/GPU/Deko3D/DekoMemoryPool.h"

namespace {

struct Backing {
	Backing(unsigned id, unsigned &live) : id(id), live(live) { ++live; }
	~Backing() { --live; }
	unsigned id;
	unsigned &live;
};

using Pool = Draw::DekoMemoryPool<std::unique_ptr<Backing>>;

void TestRetirement() {
	unsigned created = 0, live = 0;
	Pool pool;
	auto create = [&](uint32_t) { return std::make_unique<Backing>(++created, live); };
	auto first = pool.Allocate(1024, 512, 1024, create);
	assert(first && first.Offset() == 0);
	const unsigned firstBlock = first.Storage()->id;

	// An image may be destroyed on the CPU while its submission still owns the range.
	std::vector<Pool::Slice> retired;
	retired.push_back(std::move(first));
	pool.TrimEmpty();
	auto next = pool.Allocate(1024, 512, 1024, create);
	assert(next && next.Storage()->id != firstBlock && created == 2 && live == 2);

	retired.clear();
	auto reused = pool.Allocate(1024, 512, 1024, create);
	assert(reused && reused.Storage()->id == firstBlock && created == 2);
	next = std::move(reused);
	assert(!reused && next.Storage()->id == firstBlock);
	next = {};
	pool.TrimEmpty();
	assert(live == 1);
}

void TestFragmentation() {
	constexpr uint32_t slabSize = 1024 * 1024;
	unsigned created = 0, live = 0;
	Pool pool;
	auto create = [&](uint32_t size) {
		assert(size == slabSize);
		return std::make_unique<Backing>(++created, live);
	};
	auto fail = [](uint32_t) { return std::unique_ptr<Backing>{}; };
	assert(!pool.Allocate(128, 256, slabSize, fail));
	assert(!pool.Allocate(0, 256, slabSize, create));
	assert(!pool.Allocate(128, 3, slabSize, create));
	assert(!pool.Allocate(UINT32_MAX, 256, slabSize, create));
	assert(created == 0);

	struct Allocation {
		Pool::Slice slice;
		uint32_t bytes;
	};
	std::vector<Allocation> allocations;
	std::mt19937 random(3105);
	for (int i = 0; i < 12000; ++i) {
		if (!allocations.empty() && (allocations.size() == 128 || random() % 2 == 0)) {
			allocations.erase(allocations.begin() + random() % allocations.size());
			continue;
		}
		// Vary requested sizes, shader guard space, and texture/code alignment.
		const uint32_t size = 1024 + random() % 65536;
		const uint32_t align = (random() % 2 == 0) ? 256 : 512;
		auto slice = pool.Allocate(size, align, slabSize, create);
		assert(slice && slice.Offset() % align == 0 && slice.Offset() + size <= slabSize);
		for (const auto &entry : allocations) {
			if (slice.Storage() == entry.slice.Storage()) {
				assert(slice.Offset() + size <= entry.slice.Offset() ||
					entry.slice.Offset() + entry.bytes <= slice.Offset());
			}
		}
		allocations.push_back({std::move(slice), size});
	}
	assert(created < 16);
	allocations.clear();
	pool.TrimEmpty();
	assert(live == 1);
	auto full = pool.Allocate(slabSize, 512, slabSize, fail);
	assert(full && full.Offset() == 0);
}

}  // namespace

int main() {
	TestRetirement();
	TestFragmentation();
	puts("Deko memory pool: aligned ranges, reuse after retirement, coalescing, trimming and allocation failures passed");
}
