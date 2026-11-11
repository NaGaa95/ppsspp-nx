// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.

#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace Draw {

// Allocate and trim on the render thread. A slice may be released elsewhere.
// The caller retains a slice until the fence for its last GPU use signals.
template <typename Block>
class DekoMemoryPool {
	struct Range {
		uint32_t begin, end;
	};
	struct Slab {
		Slab(Block storage, uint32_t size) : block(std::move(storage)), free{{0, size}} {}
		std::optional<uint32_t> Reserve(uint32_t bytes, uint32_t alignment) {
			std::lock_guard<std::mutex> guard(mutex);
			for (auto it = free.begin(); it != free.end(); ++it) {
				const uint64_t begin = (uint64_t(it->begin) + alignment - 1) & ~(uint64_t(alignment) - 1);
				if (begin + bytes > it->end) {
					continue;
				}
				const uint32_t end = (uint32_t)begin + bytes;
				if (begin == it->begin && end == it->end) {
					free.erase(it);
				} else if (begin == it->begin) {
					it->begin = end;
				} else if (end == it->end) {
					it->end = (uint32_t)begin;
				} else {
					const uint32_t tail = it->end;
					it->end = (uint32_t)begin;
					free.insert(it + 1, {end, tail});
				}
				return (uint32_t)begin;
			}
			return {};
		}
		void Release(uint32_t begin, uint32_t bytes) {
			std::lock_guard<std::mutex> guard(mutex);
			uint32_t end = begin + bytes;
			auto next = std::lower_bound(free.begin(), free.end(), begin, [](Range range, uint32_t offset) { return range.begin < offset; });
			assert(next == free.end() || end <= next->begin);
			if (next != free.begin()) {
				auto prev = next - 1;
				assert(prev->end <= begin);
				if (prev->end == begin) {
					begin = prev->begin;
					next = free.erase(prev);
				}
			}
			if (next != free.end() && end == next->begin) {
				end = next->end;
				next = free.erase(next);
			}
			free.insert(next, {begin, end});
		}
		Block block;
		std::vector<Range> free;
		std::mutex mutex;
	};

public:
	class Slice {
	public:
		Slice() = default;
		Slice(Slice &&) noexcept = default;
		Slice &operator=(Slice &&other) noexcept {
			if (this != &other) {
				Reset();
				slab_ = std::move(other.slab_);
				offset_ = other.offset_;
				bytes_ = other.bytes_;
			}
			return *this;
		}
		~Slice() { Reset(); }
		explicit operator bool() const { return slab_ != nullptr; }
		const Block &Storage() const { return slab_->block; }
		uint32_t Offset() const { return offset_; }
	private:
		friend class DekoMemoryPool;
		Slice(std::shared_ptr<Slab> slab, uint32_t offset, uint32_t bytes)
			: slab_(std::move(slab)), offset_(offset), bytes_(bytes) {}
		void Reset() {
			if (slab_) {
				slab_->Release(offset_, bytes_);
				slab_.reset();
			}
		}
		std::shared_ptr<Slab> slab_;
		uint32_t offset_ = 0, bytes_ = 0;
	};

	template <typename Create>
	Slice Allocate(uint32_t bytes, uint32_t alignment, uint32_t slabSize, Create create) {
		if (!bytes || !alignment || (alignment & (alignment - 1)) || bytes > slabSize) {
			return {};
		}
		for (const auto &slab : slabs_) {
			if (auto offset = slab->Reserve(bytes, alignment)) {
				return Slice(slab, *offset, bytes);
			}
		}
		Block block = create(slabSize);
		if (!block) {
			return {};
		}
		auto slab = std::make_shared<Slab>(std::move(block), slabSize);
		const auto offset = slab->Reserve(bytes, alignment);
		assert(offset);
		slabs_.push_back(slab);
		return Slice(std::move(slab), *offset, bytes);
	}

	void TrimEmpty() {
		bool keptSpare = false;
		for (auto it = slabs_.begin(); it != slabs_.end();) {
			if (it->use_count() == 1) {
				if (keptSpare) {
					it = slabs_.erase(it);
					continue;
				}
				keptSpare = true;
			}
			++it;
		}
	}

private:
	std::vector<std::shared_ptr<Slab>> slabs_;
};

}  // namespace Draw
