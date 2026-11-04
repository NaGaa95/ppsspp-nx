// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.

#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <tuple>
#include <vector>

using u8 = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s64 = int64_t;
using Result = uint32_t;
constexpr int Perm_Rx = 5;
#define R_FAILED(result) ((result) != 0)
#define PPSSPP_PLATFORM_SWITCH 1
#define PPSSPP_PLATFORM_UWP 0
#define PPSSPP_PLATFORM_IOS 0
#define PPSSPP_PLATFORM_ANDROID 0
#define PPSSPP_PLATFORM_WINDOWS 0
#define PPSSPP_PLATFORM(platform) PPSSPP_PLATFORM_##platform
#define PPSSPP_ARCH(arch) 0
#ifdef TEST_MASKED
#define MASKED_PSP_MEMORY
#endif
#define MEMVIEW32_MASK 0x3FFFFFFF
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define INFO_LOG(...) ((void)0)
#define ERROR_LOG(...) ((void)0)

struct VirtmemReservation { uintptr_t address; size_t size; };
enum class Failure { None, Allocate, Base, CodeBase, Map, Permissions, View, Unmap, ViewUnmap };
static Failure failure;
static size_t allocatedSize;
static unsigned allocations, frees, codeMaps;
static unsigned viewAttempts, failViewAt = 1;
static bool codeMapped;
static std::map<uintptr_t, std::tuple<uintptr_t, size_t>> aliases;
static std::map<uintptr_t, VirtmemReservation *> reservations;
static std::vector<uintptr_t> candidates;

// Model Horizon's excluded heap/alias region, using the reported ASLR candidate.
static bool KernelRange(uintptr_t address, size_t size) {
	return address + size > address && address + size < 0x8000000000ULL &&
		!(address < 0x900000000ULL && address + size > 0x700000000ULL);
}
static bool Available(uintptr_t address, size_t size) {
	if (!KernelRange(address, size)) { return false; }
	for (const auto &[start, item] : reservations) {
		if (address < start + item->size && start < address + size) { return false; }
	}
	return true;
}
static void *FakeAllocate(size_t alignment, size_t size) {
	assert(alignment == 4096);
	if (failure == Failure::Allocate) { return nullptr; }
	assert(!allocatedSize);
	allocatedSize = size;
	++allocations;
	return (void *)0x300000000ULL;
}
static void FakeFree(void *address) {
	if (!address) { return; }
	assert(!codeMapped && aliases.empty() && allocatedSize);
	allocatedSize = 0;
	++frees;
}
static void virtmemLock() {}
static void virtmemUnlock() {}
static void *virtmemFindAslr(size_t size, size_t) {
	if (failure == Failure::Base) { return nullptr; }
	for (uintptr_t address : candidates) {
		if (Available(address, size)) { return (void *)address; }
	}
	for (uintptr_t address = 0x1000000000ULL; address < 0x4000000000ULL; address += 0x200000000ULL) {
		if (Available(address, size)) { return (void *)address; }
	}
	return nullptr;
}
static void *virtmemFindCodeMemory(size_t size, size_t) {
	assert(size == allocatedSize);
	return failure == Failure::CodeBase ? nullptr : (void *)0x400000000ULL;
}
static VirtmemReservation *virtmemAddReservation(void *address, size_t size) {
	assert(Available((uintptr_t)address, size));
	return reservations[(uintptr_t)address] = new VirtmemReservation{(uintptr_t)address, size};
}
static void virtmemRemoveReservation(VirtmemReservation *reservation) {
	assert(reservations.erase(reservation->address) == 1);
	delete reservation;
}
static unsigned envGetOwnProcessHandle() { return 1; }
static Result svcMapProcessCodeMemory(unsigned, u64, u64, size_t size) {
	assert(size == allocatedSize && !codeMapped);
	if (failure == Failure::Map) { return 0x1001; }
	codeMapped = true;
	++codeMaps;
	return 0;
}
static Result svcUnmapProcessCodeMemory(unsigned, u64, u64, size_t size) {
	assert(codeMapped && size == allocatedSize && aliases.empty());
	if (failure == Failure::Unmap) { return 0x1002; }
	codeMapped = false;
	return 0;
}
static Result svcSetProcessMemoryPermission(unsigned, uintptr_t, size_t size, int) {
	assert(size == allocatedSize && codeMapped);
	return failure == Failure::Permissions ? 0x1003 : 0;
}
static Result svcMapProcessMemory(void *target, unsigned, u64 source, size_t size) {
	assert(codeMapped && source >= 0x400000000ULL && source + size <= 0x400000000ULL + allocatedSize);
	++viewAttempts;
	if (!KernelRange((uintptr_t)target, size)) {
		fprintf(stderr, "Reproduced kernel InvalidMemoryRange 0xDC01 at dst=%p\n", target);
		return 0xDC01;
	}
	if (failure == Failure::View && viewAttempts == failViewAt) { return 0x1004; }
	assert(!aliases.count((uintptr_t)target));
	aliases[(uintptr_t)target] = {source, size};
	return 0;
}
static Result svcUnmapProcessMemory(void *target, unsigned, u64 source, size_t size) {
	assert(aliases.at((uintptr_t)target) == std::make_tuple(source, size));
	if (failure == Failure::ViewUnmap) { return 0x1005; }
	aliases.erase((uintptr_t)target);
	return 0;
}
class MemArena {
public:
	size_t roundup(size_t);
	bool NeedsProbing();
	bool GrabMemSpace(size_t);
	void ReleaseSpace();
	void *CreateView(s64, size_t, void *);
	void ReleaseView(s64, void *, size_t);
	u8 *Find4GBBase();
	static void ReleaseViewSpace(u8 *);
};
#define memalign FakeAllocate
#define free FakeFree
// ARENA_SOURCE
#undef memalign
#undef free
// BASELINE_API

static MemArena g_arena;
static u8 *base;
static u32 g_MemorySize, g_PSPModel;
constexpr int MAX_MMAP_SIZE = 31 * 1024 * 1024;
constexpr unsigned PSP_MODEL_FAT = 0, SCRATCHPAD_SIZE = 0x4000;
constexpr unsigned BOOT_PAGE_ADDRESS = 0xBFC00000, BOOT_PAGE_SIZE = 4096;
// VIEW_TYPES
static bool operator&(MemMapSetupFlags a, MemMapSetupFlags b) { return ((unsigned)a & (unsigned)b) != 0; }
static MemMapSetupFlags g_setupFlags;
static u8 *pointers[128]{};
// VIEW_TABLE
static size_t viewPositions[128];
// IGNORE_VIEW
// SKIP_VIEW
// TRY_BASE_SOURCE
// SETUP_SOURCE
// SHUTDOWN_SOURCE

static void Configure(unsigned ram, unsigned model) {
	g_MemorySize = ram * 1024 * 1024;
	g_PSPModel = model;
// CONFIGURE_VIEWS
}

int main() {
	// A 256MB search accepts this base although its PSP kernel mirror enters the excluded region.
	candidates = {0x6d0851000ULL};
	assert(g_arena.GrabMemSpace(32 * 1024 * 1024));
	base = g_arena.Find4GBBase();
	assert(base);
	void *kernelMirror = (void *)((uintptr_t)base + 0xC8000000);
	assert(g_arena.CreateView(0, 4096, kernelMirror) == kernelMirror);
	g_arena.ReleaseView(0, kernelMirror, 4096);
	g_arena.ReleaseSpace();
	candidates.clear();
	for (unsigned ram : {32, 64, 76}) {
		for (MemMapSetupFlags flags : {MemMapSetupFlags::Default, MemMapSetupFlags::AllocNullPage}) {
			Configure(ram, ram == 32 ? 0 : 1);
			assert(MemoryMap_Setup(flags));
			assert(allocatedSize < (ram + 16) * 1024 * 1024);
#ifndef MASKED_PSP_MEMORY
			const auto mainBase = base;
			const unsigned mapsBefore = codeMaps, allocationsBefore = allocations;
			for (unsigned repeat = 0; repeat < 200; ++repeat) {
				u8 *meBase = g_arena.Find4GBBase();
				assert(meBase && meBase != mainBase);
				std::vector<std::pair<void *, size_t>> meViews;
				for (size_t i = 0; i < ARRAY_SIZE(views); ++i) {
					const auto &view = views[i];
					if (!view.size || SkipView(flags, view.flags) || (view.flags & MV_MAIN_CPU_ONLY)) { continue; }
					void *mapped = g_arena.CreateView(viewPositions[i], view.size, meBase + view.virtual_address);
					assert(mapped);
					assert(std::get<0>(aliases.at((uintptr_t)mapped)) == 0x400000000ULL + viewPositions[i]);
					meViews.emplace_back(mapped, view.size);
				}
				for (auto [mapped, size] : meViews) { g_arena.ReleaseView(0, mapped, size); }
				MemArena::ReleaseViewSpace(meBase);
				assert(allocations == allocationsBefore && codeMaps == mapsBefore && reservations.size() == 2);
			}
#endif
			MemoryMap_Shutdown();
			assert(!allocatedSize && !codeMapped && aliases.empty() && reservations.empty());
		}
	}
	for (Failure fail : {Failure::Allocate, Failure::Base, Failure::CodeBase, Failure::Map, Failure::Permissions, Failure::View}) {
		Configure(32, 0);
		failure = fail;
		viewAttempts = 0;
		assert(!MemoryMap_Setup(MemMapSetupFlags::Default));
		g_arena.ReleaseSpace();
		assert(!allocatedSize && !codeMapped && aliases.empty() && reservations.empty());
	}
	failure = Failure::None;
	Configure(32, 0);
	assert(MemoryMap_Setup(MemMapSetupFlags::Default));
	const size_t mainViewCount = aliases.size();
	MemoryMap_Shutdown();
	for (failViewAt = 1; failViewAt <= mainViewCount; ++failViewAt) {
		viewAttempts = 0;
		failure = Failure::View;
		assert(!MemoryMap_Setup(MemMapSetupFlags::Default));
		assert(!allocatedSize && !codeMapped && aliases.empty() && reservations.empty());
	}
	failure = Failure::None;
	assert(MemoryMap_Setup(MemMapSetupFlags::Default));
	failure = Failure::Base;
	assert(!g_arena.Find4GBBase());
	assert(codeMapped && allocatedSize && aliases.size() == mainViewCount && reservations.size() == 2);
	failure = Failure::None;
	MemoryMap_Shutdown();
	for (unsigned i = 0; i < 200; ++i) {
		Configure(32, 0);
		assert(MemoryMap_Setup(MemMapSetupFlags::Default));
		MemoryMap_Shutdown();
	}
	assert(allocations == frees);
	assert(g_arena.GrabMemSpace(0x21000));
	base = g_arena.Find4GBBase();
	void *tracked = base + 0x10000;
	assert(g_arena.CreateView(0x1000, 4096, tracked));
	const auto reservationCount = reservations.size();
	MemArena::ReleaseViewSpace(base);
	assert(reservations.size() == reservationCount);
	failure = Failure::ViewUnmap;
	g_arena.ReleaseView(0, tracked, 4096);
	g_arena.ReleaseSpace();
	assert(codeMapped && allocatedSize && aliases.size() == 1);
	failure = Failure::None;
	g_arena.ReleaseView(0, tracked, 4096);
	g_arena.ReleaseSpace();
	assert(!allocatedSize && aliases.empty() && reservations.empty());
	assert(g_arena.GrabMemSpace(0x21000));
	assert(g_arena.Find4GBBase());
	failure = Failure::Unmap;
	g_arena.ReleaseSpace();
	assert(codeMapped && allocatedSize && frees + 1 == allocations);
	failure = Failure::None;
	g_arena.ReleaseSpace();
	assert(!codeMapped && !allocatedSize && allocations == frees && reservations.empty());
	puts("Switch memory checks passed: full PSP view table, 0xDC01 range regression, mirrored backing sizes, masked/unmasked layouts, ME isolation/recreation, allocation/map rollback, 200 relaunches, unmap failure safety.");
}
