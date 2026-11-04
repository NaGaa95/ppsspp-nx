// Copyright (C) 2023 M4xw

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

#include "ppsspp_config.h"
#if PPSSPP_PLATFORM(SWITCH)

#include <stdio.h>
#include <malloc.h> // memalign
#include <errno.h>
#include <switch.h>

#include "Common/MemArena.h"

static uintptr_t memoryBase = 0;
static uintptr_t memoryCodeBase = 0;
static uintptr_t memorySrcBase = 0;
static VirtmemReservation *memoryReservation = nullptr;
static VirtmemReservation *memoryCodeReservation = nullptr;
static bool memoryCodeMapped = false;
static size_t memoryBackingSize = 0;

static constexpr size_t PSP_ADDRESS_SPACE_SIZE = 0x10000000;

static void *ReserveVirtmem(size_t size, bool code, VirtmemReservation **reservation) {
	virtmemLock();
	void *address = code ? virtmemFindCodeMemory(size, 0x1000) : virtmemFindAslr(size, 0x1000);
	*reservation = address ? virtmemAddReservation(address, size) : nullptr;
	virtmemUnlock();
	return *reservation ? address : nullptr;
}

size_t MemArena::roundup(size_t x) {
	return (x + 0xFFF) & ~(size_t)0xFFF;
}

bool MemArena::NeedsProbing() {
	return false;
}

bool MemArena::GrabMemSpace(size_t size) {
	if (memorySrcBase || memoryCodeMapped || size == 0 || size > PSP_ADDRESS_SPACE_SIZE) {
		return false;
	}
	memoryBackingSize = roundup(size);
	errno = 0;
	memorySrcBase = (uintptr_t)memalign(0x1000, memoryBackingSize);
	return memorySrcBase != 0;
}

void MemArena::ReleaseSpace() {
	if (memoryCodeMapped) {
		Result rc = svcUnmapProcessCodeMemory(envGetOwnProcessHandle(), (u64)memoryCodeBase, (u64)memorySrcBase, memoryBackingSize);
		if (R_FAILED(rc)) {
			// The heap pages remain mapped by the kernel. Reusing them would corrupt guest memory.
			return;
		}
	}
	memoryCodeMapped = false;

	free((void *)memorySrcBase);
	memorySrcBase = 0;
	memoryBackingSize = 0;
	virtmemLock();
	if (memoryReservation) {
		virtmemRemoveReservation(memoryReservation);
	}
	if (memoryCodeReservation) {
		virtmemRemoveReservation(memoryCodeReservation);
	}
	virtmemUnlock();
	memoryReservation = nullptr;
	memoryCodeReservation = nullptr;
	memoryBase = 0;
	memoryCodeBase = 0;
}

void *MemArena::CreateView(s64 offset, size_t size, void *base) {
	if (!memoryCodeMapped || offset < 0 || (size_t)offset > memoryBackingSize || size > memoryBackingSize - (size_t)offset) {
		return nullptr;
	}
	Result rc = svcMapProcessMemory(base, envGetOwnProcessHandle(), (u64)(memoryCodeBase + offset), size);
	if (R_FAILED(rc)) {
		printf("Fatal error creating the view... base: %p offset: %p size: %p src: %p err: %d\n",
			   (void *)base, (void *)offset, (void *)size, (void *)(memoryCodeBase + offset), rc);
		return nullptr;
	}

	return base;
}

void MemArena::ReleaseView(s64 offset, void *view, size_t size) {
	Result rc = svcUnmapProcessMemory(view, envGetOwnProcessHandle(), (u64)(memoryCodeBase + offset), size);
	if (R_FAILED(rc)) {
		printf("Failed to unmap view...\n");
	}
}

u8 *MemArena::Find4GBBase() {
	if (memoryCodeMapped) {
		return (u8 *)memoryBase;
	}
	if (!memorySrcBase || !memoryBackingSize) {
		return nullptr;
	}

	if (!memoryBase) {
		memoryBase = (uintptr_t)ReserveVirtmem(PSP_ADDRESS_SPACE_SIZE, false, &memoryReservation);
	}

	if (!memoryCodeBase) {
		memoryCodeBase = (uintptr_t)ReserveVirtmem(memoryBackingSize, true, &memoryCodeReservation);
	}

	if (!memorySrcBase || !memoryBase || !memoryCodeBase) {
		ReleaseSpace();
		return nullptr;
	}

	Result rc = svcMapProcessCodeMemory(envGetOwnProcessHandle(), (u64)memoryCodeBase, (u64)memorySrcBase, memoryBackingSize);
	if (R_FAILED(rc)) {
		ReleaseSpace();
		return nullptr;
	}
	memoryCodeMapped = true;
	rc = svcSetProcessMemoryPermission(envGetOwnProcessHandle(), memoryCodeBase, memoryBackingSize, Perm_Rx);
	if (R_FAILED(rc)) {
		ReleaseSpace();
		return nullptr;
	}

	return (u8 *)memoryBase;
}

#endif // PPSSPP_PLATFORM(SWITCH)
