// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.

#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "Common/Thread/ThreadUtil.h"

// One batch may be queued or running. Results are applied by the emulation thread.
class GamedataInstallWorker {
public:
	static constexpr size_t BATCH_SIZE = 512 * 1024;
	enum class Error { None, Read, Write };
	struct Result {
		size_t copied = 0;
		Error error = Error::None;
		int64_t ioResult = 0;
	};
	using Read = std::function<int64_t(uint8_t *, size_t)>;
	using Write = std::function<int64_t(const uint8_t *, size_t)>;

	~GamedataInstallWorker() { Stop(); }

	bool Submit(size_t bytes, Read read, Write write) {
		std::lock_guard<std::mutex> lock(mutex_);
		if (pending_ || ready_ || stopping_ || !read || !write || !bytes || bytes > BATCH_SIZE) {
			return false;
		}
		if (!thread_.joinable()) {
			thread_ = std::thread(&GamedataInstallWorker::Run, this);
		}
		job_ = {bytes, std::move(read), std::move(write)};
		pending_ = true;
		wake_.notify_all();
		return true;
	}

	bool TakeResult(Result &result, bool wait = false) {
		std::unique_lock<std::mutex> lock(mutex_);
		if (wait) {
			wake_.wait(lock, [this] { return !pending_; });
		}
		if (!ready_) {
			return false;
		}
		result = result_;
		ready_ = false;
		return true;
	}

	// Drain the accepted batch before releasing its file handles or buffer.
	void Stop() {
		{
			std::lock_guard<std::mutex> lock(mutex_);
			stopping_ = true;
			wake_.notify_all();
		}
		if (thread_.joinable()) {
			thread_.join();
		}
		std::lock_guard<std::mutex> lock(mutex_);
		stopping_ = false;
	}

private:
	struct Job {
		size_t bytes = 0;
		Read read;
		Write write;
	};

	static Result Copy(const Job &job, uint8_t *buffer) {
		Result result;
		size_t loaded = 0;
		while (loaded < job.bytes) {
			const size_t remaining = job.bytes - loaded;
			const int64_t count = job.read(buffer + loaded, remaining);
			if (count <= 0 || (uint64_t)count > remaining) {
				result.error = Error::Read;
				result.ioResult = count;
				break;
			}
			loaded += (size_t)count;
		}
		while (result.copied < loaded) {
			const size_t remaining = loaded - result.copied;
			const int64_t count = job.write(buffer + result.copied, remaining);
			if (count <= 0 || (uint64_t)count > remaining) {
				result.error = Error::Write;
				result.ioResult = count;
				break;
			}
			result.copied += (size_t)count;
		}
		return result;
	}

	void Run() {
		SetCurrentThreadName("GameInstallIO");
		SetCurrentThreadAffinity(ThreadAffinityRole::IO);
		std::vector<uint8_t> buffer(BATCH_SIZE);
		std::unique_lock<std::mutex> lock(mutex_);
		while (true) {
			wake_.wait(lock, [this] { return pending_ || stopping_; });
			if (!pending_) {
				return;
			}
			Job job = std::move(job_);
			lock.unlock();
			const Result result = Copy(job, buffer.data());
			lock.lock();
			result_ = result;
			ready_ = true;
			pending_ = false;
			wake_.notify_all();
			if (stopping_) {
				return;
			}
		}
	}

	std::mutex mutex_;
	std::condition_variable wake_;
	std::thread thread_;
	Job job_;
	Result result_;
	bool pending_ = false, ready_ = false, stopping_ = false;
};
