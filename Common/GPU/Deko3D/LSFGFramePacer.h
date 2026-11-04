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
			rate_ = requested && samples_ >= 8 ? (average_ < 30'000'000 ? 1 : 0) : -1;
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
			rate_ = average_ < 30'000'000 ? 1 : 0;
		}
		if (rate_ == 1) {
			slow_ = interval >= 31'500'000 ? slow_ + 1 : interval ? 0 : slow_;
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
	void Reset() { *this = {}; }
	double SourceFPS() const { return average_ > 0 ? 1'000'000'000.0 / average_ : 0; }

private:
	uint64_t last_ = 0;
	double average_ = 0;
	unsigned samples_ = 0, slow_ = 0;
	int rate_ = -1;
	bool requested_ = false;
};

} // namespace Deko3DLSFG
