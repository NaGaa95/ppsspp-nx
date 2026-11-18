// Copyright (c) 2012- PPSSPP Project.
// Frame pacing follows Cemu-NX's native LSFG integration (GPL-2.0-or-later).
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.

#pragma once

#include <algorithm>
#include <cstdint>

namespace Deko3DLSFG {

class FramePacer {
public:
	struct Decision {
		bool generate = false, reset = false, highRate = false;
	};
	// The display's refresh rate: generation doubles sources up to half of it (30 fps at 60 Hz, 60 fps at 120 Hz).
	void SetRefreshRate(int hz) {
		const uint64_t period = hz > 0 ? 1'000'000'000ull / (uint64_t)hz : 16'666'667ull;
		if (period != period_) {
			Reset();
			period_ = period;
		}
	}
	Decision Observe(uint64_t now, bool requested, bool ready) {
		Decision result;
		const uint64_t interval = last_ && now > last_ ? now - last_ : 0;
		last_ = now;
		if (!requested || rate_ != 0) {
			if (interval >= 4'000'000 && interval <= 100'000'000) {
				average_ = samples_ ? average_ * .875 + interval * .125 : (double)interval;
				samples_ = std::min(samples_ + 1, 120u);
			}
		}
		if (requested != requested_) {
			requested_ = requested;
			rate_ = requested && samples_ >= 8 ? (average_ < FastLimit() ? 1 : 0) : -1;
			slow_ = 0;
			result.reset = true;
		}
		if (!requested || !ready) {
			return result;
		}
		if (interval > 250'000'000) {
			result.reset = true;
			return result;
		}
		if (rate_ < 0 && samples_ >= 8) {
			rate_ = average_ < FastLimit() ? 1 : 0;
		}
		if (rate_ == 1) {
			slow_ = interval >= SlowLimit() ? slow_ + 1 : interval ? 0 : slow_;
			if (slow_ < 16) {
				result.highRate = true;
				return result;
			}
			rate_ = 0;
			slow_ = 0;
			result.reset = true;
		}
		result.generate = rate_ == 0;
		return result;
	}
	void Reset() {
		const uint64_t period = period_;
		*this = {};
		period_ = period;
	}
	double SourceFPS() const { return average_ > 0 ? 1'000'000'000.0 / average_ : 0; }

private:
	// 30 ms and 31.5 ms at 60 Hz.
	uint64_t FastLimit() const { return period_ * 9 / 5; }
	uint64_t SlowLimit() const { return period_ * 189 / 100; }

	uint64_t period_ = 16'666'667;
	uint64_t last_ = 0;
	double average_ = 0;
	unsigned samples_ = 0, slow_ = 0;
	int rate_ = -1;
	bool requested_ = false;
};

} // namespace Deko3DLSFG
