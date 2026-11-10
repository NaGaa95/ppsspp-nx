#!/usr/bin/env python3
# Copyright (c) 2012- PPSSPP Project.
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2 or later.

"""Exercise production Switch underrun fades without the audio device."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
header = (root / 'Core/HW/StereoResampler.h').read_text()
source = (root / 'Core/HW/StereoResampler.cpp').read_text()

def definition(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

fixture = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <vector>
// IMPLEMENTATION

int main() {
	{
		int16_t buffer_[8]{};
		buffer_[6] = 12000; buffer_[7] = -6000;
		int16_t output[8]{};
		auto *samples = output;
		unsigned currentSample = 0, numSamples = 4, indexR = 0;
		const unsigned INDEX_MASK = 7;
		auto clamp_s16 = [](int value) { return (int16_t)std::clamp(value, -32768, 32767); };
		// CLASSIC_PADDING
		for (unsigned i = 0; i < 4; ++i) { assert(output[i * 2] == 12000 && output[i * 2 + 1] == -6000); }
	}
	for (int rate : {44100, 48000, 96000}) {
		SwitchAudioGapFade fade;
		const unsigned length = std::clamp(rate / 200, 1, 512);
		std::vector<int16_t> data(2048 * 2);
		for (unsigned i = 0; i < 2048; ++i) { data[i * 2] = 12000; data[i * 2 + 1] = -6000; }
		const auto original = data;
		fade.Process(data.data(), 2048, 2048, rate);
		assert(data == original);
		fade.Process(data.data(), 10, 2048, rate);
		for (unsigned i = 0; i < 10; ++i) { assert(data[i * 2] == 12000 && data[i * 2 + 1] == -6000); }
		for (unsigned i = 10; i < 10 + length; ++i) {
			assert(data[i * 2] >= 0 && data[i * 2 + 1] <= 0);
			assert(data[i * 2] <= data[(i - 1) * 2] && data[i * 2 + 1] >= data[(i - 1) * 2 + 1]);
		}
		for (unsigned i = 10 + length; i < 2048; ++i) { assert(data[i * 2] == 0 && data[i * 2 + 1] == 0); }
		// Continued starvation must stay silent instead of restarting the old sample.
		std::fill(data.begin(), data.end(), 12345);
		fade.Process(data.data(), 0, 2048, rate);
		assert(std::all_of(data.begin(), data.end(), [](int16_t s) { return s == 0; }));
		data = original;
		fade.Process(data.data(), 2048, 2048, rate);
		assert(data[0] > 0 && data[0] < 12000 && data[1] < 0 && data[1] > -6000);
		for (unsigned i = length; i < 2048; ++i) { assert(data[i * 2] == 12000 && data[i * 2 + 1] == -6000); }
		// A short gap and another gap during recovery must remain bounded.
		fade.Process(data.data(), 0, 1, rate);
		data[0] = -32768; data[1] = 32767;
		fade.Process(data.data(), 1, 2, rate);
		assert(data[0] > -32768 && data[0] < 12000);
		fade.Process(data.data(), 0, 2048, rate);
		assert(data.back() == 0);
		fade.Reset();
		fade.Process(nullptr, 0, 0, rate);
		fade.Process(data.data(), 0, 2048, rate);
		assert(std::all_of(data.begin(), data.end(), [](int16_t s) { return s == 0; }));
		data = original;
		fade.Reset();
		fade.Process(data.data(), 2048, 2048, rate);
		assert(data == original);
	}
	puts("Switch audio gap tests passed: unchanged normal audio, stereo channels, fade to silence, repeated starvation, recovery, interrupted recovery and reset.");
}
'''
padding = source.split('// Padding with the last value to reduce clicking', 1)[1].split('#endif', 1)[0]
fixture = fixture.replace('// CLASSIC_PADDING', padding)
fixture = fixture.replace('// IMPLEMENTATION', definition(header, 'class SwitchAudioGapFade {') + ';\n' +
                          definition(source, 'void SwitchAudioGapFade::Process('))
assert 'gapFade_.Process(samples, currentSample / 2, numSamples, sample_rate);' in source
assert 'gapResetPending_.store(true, std::memory_order_release);' in definition(source, 'void StereoResampler::Clear()')
assert 'gapResetPending_.exchange(false, std::memory_order_acquire)' in definition(source, 'void StereoResampler::Mix(')
with tempfile.TemporaryDirectory(prefix='ppsspp-audio-gap-') as directory:
    cpp, executable = Path(directory) / 'test.cpp', Path(directory) / 'test'
    cpp.write_text(fixture)
    subprocess.run(['clang++', '-std=c++17', '-O1', '-g', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', str(cpp), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
