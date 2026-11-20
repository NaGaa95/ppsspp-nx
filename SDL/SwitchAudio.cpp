// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

#include "SDL/SwitchAudio.h"

#include <switch.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "Common/Log.h"
#include "Common/System/NativeApp.h"
#include "Common/Thread/ThreadUtil.h"

namespace SwitchAudio {
namespace {

// Three periods keep 21 to 32 ms queued (devkitPro's SDL driver double-buffers whole SDL periods).
constexpr int kBufferCount = 3;
constexpr size_t kDataBytes = kPeriodFrames * 2 * sizeof(int16_t);
constexpr size_t kBufferBytes = (kDataBytes + 0xFFF) & ~size_t(0xFFF);

AudioOutBuffer g_buffers[kBufferCount];
std::thread g_thread;
std::atomic<bool> g_running{false};

void Submit(AudioOutBuffer *buffer) {
	NativeMix(static_cast<short *>(buffer->buffer), kPeriodFrames, kSampleRate, nullptr);
	armDCacheFlush(buffer->buffer, kDataBytes);
	const Result rc = audoutAppendAudioOutBuffer(buffer);
	if (R_FAILED(rc)) {
		ERROR_LOG(Log::Audio, "audoutAppendAudioOutBuffer failed: 0x%x", rc);
	}
}

void Run() {
	SetCurrentThreadName("SwitchAudio");
	SetCurrentThreadAffinity(ThreadAffinityRole::AUDIO);
	while (g_running) {
		AudioOutBuffer *released = nullptr;
		u32 count = 0;
		if (R_FAILED(audoutWaitPlayFinish(&released, &count, 100'000'000ULL))) {
			continue;
		}
		// Buffers that finish together signal the event once: refill all of them.
		while (count && released && g_running) {
			Submit(released);
			released = nullptr;
			count = 0;
			if (R_FAILED(audoutGetReleasedAudioOutBuffer(&released, &count))) {
				break;
			}
		}
	}
}

}  // namespace

bool Start() {
	if (g_running) {
		return true;
	}
	// Kept for the process: a restart reuses them.
	for (AudioOutBuffer &buffer : g_buffers) {
		if (!buffer.buffer && !(buffer.buffer = aligned_alloc(0x1000, kBufferBytes))) {
			ERROR_LOG(Log::Audio, "Could not allocate the audout buffers");
			return false;
		}
	}

	Result rc = audoutInitialize();
	if (R_FAILED(rc)) {
		ERROR_LOG(Log::Audio, "audoutInitialize failed: 0x%x", rc);
		return false;
	}
	if (audoutGetSampleRate() != (u32)kSampleRate || audoutGetChannelCount() != 2 || audoutGetPcmFormat() != PcmFormat_Int16) {
		ERROR_LOG(Log::Audio, "audout plays %u Hz, %u channels, format %d", audoutGetSampleRate(), audoutGetChannelCount(), (int)audoutGetPcmFormat());
		audoutExit();
		return false;
	}
	rc = audoutStartAudioOut();
	if (R_FAILED(rc)) {
		ERROR_LOG(Log::Audio, "audoutStartAudioOut failed: 0x%x", rc);
		audoutExit();
		return false;
	}

	for (AudioOutBuffer &buffer : g_buffers) {
		buffer.next = nullptr;
		buffer.buffer_size = kBufferBytes;
		buffer.data_size = kDataBytes;
		buffer.data_offset = 0;
		memset(buffer.buffer, 0, kDataBytes);
		armDCacheFlush(buffer.buffer, kDataBytes);
		rc = audoutAppendAudioOutBuffer(&buffer);
		if (R_FAILED(rc)) {
			ERROR_LOG(Log::Audio, "audoutAppendAudioOutBuffer failed: 0x%x", rc);
		}
	}

	g_running = true;
	g_thread = std::thread(Run);
	INFO_LOG(Log::Audio, "audout: %d Hz, %d buffers of %d frames", kSampleRate, kBufferCount, kPeriodFrames);
	return true;
}

void Stop() {
	if (!g_running) {
		return;
	}
	g_running = false;
	g_thread.join();
	audoutStopAudioOut();
	// Closing the session hands its buffers back.
	audoutExit();
}

}  // namespace SwitchAudio
