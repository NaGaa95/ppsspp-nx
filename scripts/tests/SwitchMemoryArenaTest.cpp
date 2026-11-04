// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.

#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <tuple>

using u8 = uint8_t;
using u64 = uint64_t;
using s64 = int64_t;
using Result = uint32_t;
constexpr int Perm_Rx = 5;
#define R_FAILED(result) ((result) != 0)
#define PPSSPP_PLATFORM_SWITCH 1
#define PPSSPP_PLATFORM_UWP 0
#define PPSSPP_PLATFORM(platform) PPSSPP_PLATFORM_##platform
#define MASKED_PSP_MEMORY
#define MEMVIEW32_MASK 0x0FFFFFFF
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

struct VirtmemReservation { void *address; };
enum class Failure { None, Allocate, Base, CodeBase, Map, Permissions, View, Unmap };
static Failure failure;
static size_t allocatedSize;
static unsigned allocations, frees;
static bool codeMapped;
static std::map<uintptr_t, std::tuple<uintptr_t, size_t>> aliases;
static std::map<void *, VirtmemReservation *> reservations;

static void *FakeAllocate(size_t alignment, size_t size) {
	assert(alignment == 4096);
	if (failure == Failure::Allocate) { return nullptr; }
	assert(allocatedSize == 0);
	allocatedSize = size;
	++allocations;
	return (void *)0x100000000ULL;
}
static void FakeFree(void *address) {
	if (!address) { return; }
	assert(!codeMapped && aliases.empty());
	assert(allocatedSize);
	allocatedSize = 0;
	++frees;
}
static void virtmemLock() {}
static void virtmemUnlock() {}
static void *virtmemFindAslr(size_t size, size_t) {
	assert(size == 0x10000000);
	return failure == Failure::Base ? nullptr : (void *)0x800000000ULL;
}
static void *virtmemFindCodeMemory(size_t size, size_t) {
	assert(size == allocatedSize);
	return failure == Failure::CodeBase ? nullptr : (void *)0x400000000ULL;
}
static VirtmemReservation *virtmemAddReservation(void *address, size_t) {
	assert(!reservations.count(address));
	return reservations[address] = new VirtmemReservation{address};
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
	assert(codeMapped);
	assert(source >= 0x400000000ULL && source + size <= 0x400000000ULL + allocatedSize);
	if (failure == Failure::View) { return 0x1004; }
	aliases[(uintptr_t)target] = {source, size};
	return 0;
}
static Result svcUnmapProcessMemory(void *target, unsigned, u64 source, size_t size) {
	assert(aliases.at((uintptr_t)target) == std::make_tuple(source, size));
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
};
#define memalign FakeAllocate
#define free FakeFree
// ARENA_SOURCE
#undef memalign
#undef free

static MemArena g_arena;
static u8 *base;
static bool Memory_TryBase(int) { return false; }
static bool SetupTail(int flags) {
// SETUP_TAIL

enum class MemMapSetupFlags { Default };
static MemMapSetupFlags g_setupFlags;
constexpr int MV_MIRROR_PREVIOUS = 1;
struct MemoryView { u8 **out_ptr; uint32_t virtual_address; size_t size; int flags; };
static u8 *primaryA, *mirrorA, *primaryB, *mirrorB, *lowMirrorB;
static MemoryView views[] = {
	{&primaryA, 0x08800000, 4096, 0}, {&mirrorA, 0x48800000, 4096, MV_MIRROR_PREVIOUS},
	{&primaryB, 0x04000000, 4096, 0}, {&lowMirrorB, 0x04200000, 4096, MV_MIRROR_PREVIOUS},
	{&mirrorB, 0x44200000, 4096, MV_MIRROR_PREVIOUS},
};
static bool SkipView(MemMapSetupFlags, int) { return false; }
// IGNORE_VIEW
// SHUTDOWN_SOURCE

int main() {
	for (unsigned ram : {32, 64, 76}) {
		const size_t requested = ram * 1024 * 1024 + 0x21000;
		assert(g_arena.GrabMemSpace(requested));
		assert(allocatedSize == requested);
		auto *address = g_arena.Find4GBBase();
		assert(address);
		const unsigned before = allocations;
		assert(g_arena.Find4GBBase() == address && allocations == before);
		void *a = (void *)0x808000000ULL, *b = (void *)0x804000000ULL;
		assert(g_arena.CreateView(0, 4096, a) == a);
		assert(g_arena.CreateView(0, 4096, b) == b);
		assert(aliases.at((uintptr_t)a) == aliases.at((uintptr_t)b));
		assert(!g_arena.CreateView(requested, 4096, a));
		assert(!g_arena.CreateView(-1, 4096, a));
		g_arena.ReleaseView(0, b, 4096);
		g_arena.ReleaseView(0, a, 4096);
		g_arena.ReleaseSpace();
		g_arena.ReleaseSpace();
		assert(!allocatedSize && !codeMapped && reservations.empty());
	}
	for (Failure fail : {Failure::Allocate, Failure::Base, Failure::CodeBase, Failure::Map, Failure::Permissions}) {
		failure = fail;
		const bool grabbed = g_arena.GrabMemSpace(0x21000);
		assert(grabbed == (fail != Failure::Allocate));
		if (grabbed) { assert(!g_arena.Find4GBBase()); }
		g_arena.ReleaseSpace();
		assert(!allocatedSize && !codeMapped && reservations.empty());
	}
	failure = Failure::None;
	assert(g_arena.GrabMemSpace(0x21000));
	assert(g_arena.Find4GBBase());
	primaryA = mirrorA = (u8 *)0x808000000ULL;
	primaryB = (u8 *)0x804000000ULL;
	lowMirrorB = mirrorB = (u8 *)0x804200000ULL;
	assert(g_arena.CreateView(0, 4096, primaryA));
	assert(g_arena.CreateView(4096, 4096, primaryB));
	assert(g_arena.CreateView(4096, 4096, lowMirrorB));
	assert(aliases.at((uintptr_t)primaryB) == aliases.at((uintptr_t)lowMirrorB));
	MemoryMap_Shutdown();
	assert(!primaryA && !primaryB && !mirrorA && !mirrorB && !lowMirrorB);
	assert(aliases.empty() && !allocatedSize && reservations.empty());
	assert(g_arena.GrabMemSpace(0x21000));
	base = g_arena.Find4GBBase();
	failure = Failure::View;
	assert(!g_arena.CreateView(0, 4096, (void *)0x808000000ULL));
	assert(base && !SetupTail(0));
	assert(!base && !allocatedSize && !codeMapped && reservations.empty());
	failure = Failure::None;
	for (unsigned i = 0; i < 200; ++i) {
		assert(g_arena.GrabMemSpace(0x21000));
		assert(g_arena.Find4GBBase());
		g_arena.ReleaseSpace();
	}
	assert(allocations == frees);
	assert(g_arena.GrabMemSpace(0x21000));
	assert(g_arena.Find4GBBase());
	failure = Failure::Unmap;
	g_arena.ReleaseSpace();
	assert(codeMapped && allocatedSize && frees + 1 == allocations);
	failure = Failure::None;
	g_arena.ReleaseSpace();
	assert(!codeMapped && !allocatedSize && allocations == frees && reservations.empty());
	puts("Switch memory checks passed: exact backing sizes, shared aliases, masked-mirror shutdown, allocation/map rollback, failed setup cleanup, 200 relaunches, unmap failure safety.");
}
