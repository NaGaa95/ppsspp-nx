#!/usr/bin/env python3
# Copyright (c) 2012- PPSSPP Project.
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2 or later.

"""Test the launcher's actual artwork queue/upload and nonblocking cache methods.

Uses real mutex contention and a fake SDL texture sink; no Switch SDK is needed.
Run on Linux with a C++17 compiler and address/undefined-behavior sanitizers.
"""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def body(source, marker):
    start = source.index("{", source.index(marker))
    depth, end = 1, start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    root = Path(__file__).resolve().parent.parent
    launcher = (root / "SDL/SwitchLauncher.cpp").read_text()
    cache = (root / "UI/GameInfoCache.cpp").read_text()
    header = (root / "UI/GameInfoCache.h").read_text()
    config = (root / "Core/Config.cpp").read_text()
    fixture = r'''
#include "Common/Common.h"
#include "SDL/SwitchLauncherUtil.h"
#include <cassert>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
using namespace SwitchLauncher;
#undef PPSSPP_PLATFORM
#define PPSSPP_PLATFORM(name) (PLATFORM_##name)
#define PLATFORM_SWITCH 1
enum class GPUBackend { OPENGL, VULKAN, DEKO3D };
bool IsVREnabled() { return false; }
static int DefaultGPUBackend() @DEFAULT_BACKEND@
enum class GameRegion { UNKNOWN, USA };
enum class TaskPriority { LOW, NORMAL, HIGH };
enum class GameInfoFlags @FLAGS@;
ENUM_CLASS_BITOPS(GameInfoFlags);
struct GameInfoMetadata @METADATA@;
struct Path {
	std::string value;
	Path() = default;
	explicit Path(std::string text) : value(std::move(text)) {}
	bool empty() const { return value.empty(); }
	const std::string &ToString() const { return value; }
	std::string GetFilename() const { return value.substr(value.find_last_of('/') + 1); }
};
struct GameInfo {
	std::mutex lock;
	GameInfoFlags hasFlags{}, pendingFlags{};
	std::string title, id;
	GameRegion region = GameRegion::UNKNOWN;
	bool hasConfig = false;
	double lastAccessedTime = 0;
	Path filePath_;
	explicit GameInfo(const Path &path) : filePath_(path) {}
	const Path &GetFilePath() const { return filePath_; }
	bool TryGetMetadata(GameInfoMetadata *metadata) @TRY_METADATA@
};
struct GameInfoWorkItem {
	std::shared_ptr<GameInfo> info;
	GameInfoFlags flags;
	TaskPriority priority;
	GameInfoWorkItem(const Path &, std::shared_ptr<GameInfo> &i, GameInfoFlags f, TaskPriority p) : info(i), flags(f), priority(p) {}
};
struct ThreadManager {
	std::vector<std::unique_ptr<GameInfoWorkItem>> tasks;
	void EnqueueTask(GameInfoWorkItem *task) { tasks.emplace_back(task); }
} g_threadManager;
double time_now_d() { return 1.0; }
struct GameInfoCache {
	std::mutex mapLock_;
	std::map<std::string, std::shared_ptr<GameInfo>> info_;
	std::shared_ptr<GameInfo> TryGetInfo(const Path &gamePath, GameInfoFlags wantFlags, GameInfoFlags *outHasFlags = nullptr,
		TaskPriority priority = TaskPriority::NORMAL) @TRY_INFO@
} cache;
GameInfoCache *g_gameInfoCache = &cache;
struct Entry {
	std::string key;
	Path path;
	std::shared_ptr<GameInfo> info;
};
struct Decode @DECODE@;
struct Pixels @PIXELS@;
constexpr size_t ARTWORK_UPLOAD_BYTES = 512 * 1024;
constexpr size_t MAX_ARTWORK = 64;
constexpr const char *ART_DIRECTORY = "/covers";
constexpr int SDL_PIXELFORMAT_RGBA32 = 1, SDL_TEXTUREACCESS_STATIC = 1, SDL_BLENDMODE_BLEND = 1;
struct SDL_Rect { int x, y, w, h; };
struct SDL_Texture {
	int width, height;
	std::vector<uint8_t> data;
};
int liveTextures = 0, uploads = 0;
bool failUpload = false;
SDL_Texture *SDL_CreateTexture(void *, int, int, int w, int h) {
	++liveTextures;
	return new SDL_Texture{w, h, std::vector<uint8_t>((size_t)w * h * 4, 0)};
}
void SDL_DestroyTexture(SDL_Texture *texture) {
	if (texture) {
		--liveTextures;
		delete texture;
	}
}
void SDL_SetTextureBlendMode(SDL_Texture *, int) {}
bool SDL_UpdateTexture(SDL_Texture *texture, const SDL_Rect *rect, const void *data, int pitch) {
	assert(++uploads == 1);
	assert(rect && rect->x == 0 && rect->y >= 0 && rect->w == texture->width && rect->h > 0);
	assert(rect->y + rect->h <= texture->height && pitch == texture->width * 4);
	assert((size_t)rect->h * pitch <= ARTWORK_UPLOAD_BYTES);
	if (failUpload) { return false; }
	memcpy(texture->data.data() + (size_t)rect->y * pitch, data, (size_t)rect->h * pitch);
	return true;
}
struct Texture { SDL_Texture *texture; uint64_t use; };
struct Launcher {
	std::set<std::string> pendingArt_;
	std::unordered_map<std::string, uint64_t> artRevisions_;
	std::deque<Decode> decodeQueue_, metadataQueue_;
	std::deque<Pixels> decoded_;
	Pixels uploading_;
	SDL_Texture *uploadTexture_ = nullptr;
	int uploadRow_ = 0;
	std::mutex decodeMutex_;
	std::condition_variable decodeWake_;
	void *renderer_ = nullptr;
	uint64_t use_ = 0;
	std::unordered_map<std::string, Texture> artwork_;
	void QueueArtwork(Entry &entry, bool background) @QUEUE@
	void PumpArtwork() @PUMP@
	void InvalidateArtwork(Entry &entry) @INVALIDATE@
	void Pump() { uploads = 0; PumpArtwork(); }
	void Complete(std::string key, int w, int h, uint8_t value = 77) {
		Pixels p;
		p.key = key;
		p.revision = artRevisions_[key];
		p.width = w;
		p.height = h;
		p.data.assign((size_t)w * h * 4, value);
		pendingArt_.insert(key);
		decoded_.push_back(std::move(p));
	}
	~Launcher() {
		SDL_DestroyTexture(uploadTexture_);
		for (auto &[key, image] : artwork_) { SDL_DestroyTexture(image.texture); }
	}
};
void WhileBusy(std::mutex &mutex, std::function<void()> poll) {
	std::promise<void> locked, release;
	auto released = release.get_future();
	std::thread worker([&] {
		std::lock_guard<std::mutex> guard(mutex);
		locked.set_value();
		released.wait();
	});
	locked.get_future().wait();
	poll();
	release.set_value();
	worker.join();
}
void TestCache() {
	const Path path("/games/slow.iso");
	GameInfoFlags ready = GameInfoFlags::ICON;
	WhileBusy(cache.mapLock_, [&] {
		assert(!cache.TryGetInfo(path, GameInfoFlags::ICON, &ready));
		assert(ready == GameInfoFlags::EMPTY);
	});
	auto info = cache.TryGetInfo(path, GameInfoFlags::PARAM_SFO);
	assert(info && g_threadManager.tasks.size() == 1);
	for (int i = 0; i < 100; ++i) { cache.TryGetInfo(path, GameInfoFlags::PARAM_SFO); }
	assert(g_threadManager.tasks.size() == 1);
	GameInfoMetadata metadata;
	assert(!info->TryGetMetadata(&metadata));
	WhileBusy(info->lock, [&] {
		assert(cache.TryGetInfo(path, GameInfoFlags::ICON, &ready) == info);
		assert(ready == GameInfoFlags::EMPTY && !info->TryGetMetadata(&metadata));
	});
	cache.TryGetInfo(path, GameInfoFlags::ICON);
	assert(g_threadManager.tasks.size() == 2);
	assert(g_threadManager.tasks.back()->flags & GameInfoFlags::FILE_TYPE);
	info->hasFlags = GameInfoFlags::FILE_TYPE | GameInfoFlags::PARAM_SFO | GameInfoFlags::ICON;
	info->pendingFlags &= ~info->hasFlags;
	info->id = "ULUS12345";
	info->hasConfig = true;
	assert(info->TryGetMetadata(&metadata) && metadata.title == "slow.iso" && metadata.hasConfig);
	info->title = "Loaded title";
	assert(info->TryGetMetadata(&metadata) && metadata.title == info->title && metadata.id == info->id);
	cache.TryGetInfo(path, GameInfoFlags::ICON, &ready);
	assert((ready & GameInfoFlags::ICON) && g_threadManager.tasks.size() == 2);
}
void TestDeferredAndMissing() {
	Launcher l;
	Entry e{"missing", Path("/games/missing.iso"), {}};
	const auto before = g_threadManager.tasks.size();
	WhileBusy(l.decodeMutex_, [&] { l.QueueArtwork(e, false); });
	assert(l.pendingArt_.empty());
	l.QueueArtwork(e, false);
	l.QueueArtwork(e, false);
	assert(l.decodeQueue_.size() == 1 && g_threadManager.tasks.size() == before);
	// The worker found no custom cover; metadata can still be busy or unfinished.
	l.metadataQueue_.push_back(std::move(l.decodeQueue_.front()));
	l.decodeQueue_.pop_front();
	WhileBusy(cache.mapLock_, [&] { l.Pump(); });
	assert(l.metadataQueue_.size() == 1);
	for (int i = 0; i < 100; ++i) { l.Pump(); }
	assert(l.metadataQueue_.size() == 1 && g_threadManager.tasks.size() == before + 1);
	auto info = cache.info_.at(e.path.ToString());
	info->hasFlags = GameInfoFlags::FILE_TYPE | GameInfoFlags::PARAM_SFO | GameInfoFlags::ICON;
	info->pendingFlags = GameInfoFlags::EMPTY;
	WhileBusy(info->lock, [&] { l.Pump(); });
	assert(l.metadataQueue_.size() == 1);
	l.Pump();
	assert(l.metadataQueue_.empty() && l.decodeQueue_.size() == 1);
	assert(l.decodeQueue_.front().info == info);
	l.decodeQueue_.clear();
	l.Complete("missing.cover", 0, 0);
	l.Pump();
	assert(l.artwork_.count("missing.cover") && !l.artwork_.at("missing.cover").texture);
	l.QueueArtwork(e, false);
	assert(l.decodeQueue_.empty() && l.pendingArt_.empty());
}
void TestUploads() {
	Launcher l;
	l.Complete("large.background", 1280, 720);
	l.Complete("next.cover", 144, 80);
	int frames = 0;
	do {
		l.Pump();
		assert(++frames <= 8 && !l.artwork_.count("next.cover"));
	} while (!l.artwork_.count("large.background"));
	assert(frames == 8 && l.pendingArt_.count("next.cover"));
	for (uint8_t pixel : l.artwork_.at("large.background").texture->data) { assert(pixel == 77); }
	l.Pump();
	assert(l.artwork_.count("next.cover") && l.pendingArt_.empty());
	l.Complete("failed.cover", 144, 80);
	failUpload = true;
	l.Pump();
	failUpload = false;
	assert(!l.artwork_.at("failed.cover").texture && l.pendingArt_.empty());
	for (int i = 0; i < 70; ++i) {
		l.Complete("cover" + std::to_string(i), 8, 8);
		l.Pump();
	}
	assert(l.artwork_.size() == MAX_ARTWORK && liveTextures == (int)MAX_ARTWORK);
}
void TestReplacement() {
	Launcher l;
	Entry e{"replace", Path("/games/game.iso"), {}};
	l.Complete("replace.cover", 480, 720);
	l.Pump();
	assert(l.uploadTexture_ && !l.artwork_.count("replace.cover"));
	l.InvalidateArtwork(e);
	l.Complete("replace.cover", 144, 80, 42);
	l.Pump();
	assert(!l.uploadTexture_ && l.artwork_.at("replace.cover").texture->data.front() == 42);
	l.Complete("replace.cover", 144, 80, 9);
	l.InvalidateArtwork(e);
	l.QueueArtwork(e, false);
	l.Pump();
	assert(!l.artwork_.count("replace.cover") && l.pendingArt_.count("replace.cover"));
	l.decodeQueue_.clear();
	l.Complete("replace.cover", 144, 80, 99);
	l.Pump();
	assert(l.artwork_.at("replace.cover").texture->data.front() == 99 && l.pendingArt_.empty());
}
int main() {
	assert(DefaultGPUBackend() == (int)GPUBackend::DEKO3D);
	TestCache();
	TestDeferredAndMissing();
	TestUploads();
	TestReplacement();
	assert(liveTextures == 0);
	puts("Switch artwork tests passed: busy-lock polling, request coalescing, missing icons, bounded uploads, "
		"upload failure, LRU eviction, replacement during upload, stale-result rejection.");
}
'''
    methods = {
        "DEFAULT_BACKEND": body(config, "static int DefaultGPUBackend("),
        "FLAGS": body(header, "enum class GameInfoFlags"),
        "METADATA": body(header, "struct GameInfoMetadata"),
        "TRY_METADATA": body(cache, "bool GameInfo::TryGetMetadata("),
        "TRY_INFO": body(cache, "std::shared_ptr<GameInfo> GameInfoCache::TryGetInfo("),
        "DECODE": body(launcher, "struct Decode"),
        "PIXELS": body(launcher, "struct Pixels"),
        "QUEUE": body(launcher, "void Launcher::QueueArtwork("),
        "PUMP": body(launcher, "void Launcher::PumpArtwork("),
        "INVALIDATE": body(launcher, "void Launcher::InvalidateArtwork("),
    }
    for name, implementation in methods.items():
        fixture = fixture.replace(f"@{name}@", implementation)
    with tempfile.TemporaryDirectory(prefix="ppsspp-artwork-") as directory:
        test = Path(directory) / "test.cpp"
        test.write_text(fixture)
        executable = Path(directory) / "test"
        compiler = shlex.split(os.environ.get("CXX", "c++"))
        subprocess.run(compiler + ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-O1", "-g", "-pthread",
                                  "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I", str(root),
                                  str(test), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True, timeout=20)


if __name__ == "__main__":
    main()
