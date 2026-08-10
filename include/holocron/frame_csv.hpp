// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/frame_csv.hpp
//
// One `AudioFrame`, written as one line of CSV.
//
// WHY THIS IS ITS OWN MODULE AND NOT A FUNCTION IN THE HARNESS.
//
// M1's exit criterion asks that the analysis "runs headless against a fixture
// and produces output diffable against a golden file". The obvious build is a
// test that formats frames its own way and compares against a file. That test
// passes forever while `holocron-analyze --csv` -- the thing anyone actually
// runs to check an analysis change -- drifts away from it unnoticed, because
// nothing compares the two.
//
// So the harness and the golden-file test call THIS, and the golden is the
// harness's own output format. A column added here appears in both, and the
// golden fails until it is regenerated, which is the reminder.
//
// -- what a column is chosen for ----------------------------------------------
//
// Every scalar on `AudioFrame` that a crystal can bind to has a column. The six
// ARRAYS -- `band`, `band_env`, `band_norm`, `fft_magnitude`, `fft_smoothed`,
// `waveform` -- would be 3,000 columns a row, so each is reduced to three
// numbers: `max`, `rms`, and `centroid`, the last being where the energy sits as
// a fraction of the array's length.
//
// THE CENTROID IS THERE INSTEAD OF AN ARGMAX, which is the reduction that first
// suggests itself and is the wrong one. An argmax is an integer and would be
// compared exactly, and an integer chosen by comparing floats FLIPS when two
// near-equal elements disagree in the last bit -- which is exactly what MSVC and
// gcc do. The whole golden would then be flaky on one column for a reason that
// has nothing to do with the analysis. A centroid moves smoothly, is compared
// with the same tolerance as everything else, and still says the energy moved.
//
// A SINGLE ELEMENT'S CHANGE IS DILUTED BY THE ARRAY LENGTH, and that is admitted
// rather than solved: one bin of 1,024 shifting by 0.01 moves `rms` by about
// 1e-5. Any one number summarising 1,024 has that property. What it does catch
// is the whole class of regression that actually happens -- a changed window, a
// changed band edge, a changed envelope constant -- which moves every element at
// once.
//
// -- and why it formats into a caller's buffer --------------------------------
//
// A whole track is ~5,600 frames, and returning a `std::string` per frame is
// 5,600 allocations to write a file. Nothing here is in the audio path, so that
// would be legal and merely wasteful; a caller-provided buffer costs one line at
// the call site and makes the harness's inner loop allocation-free by
// construction.

#pragma once

#include <holocron/audio_frame.hpp>

#include <cstddef>

namespace holocron {

// Big enough for any row this writes, with room for a column or two more.
constexpr std::size_t kFrameCsvRowMax = 2048;

// The column names, comma separated, with a trailing newline. Never null.
const char* frame_csv_header();

// Write `frame` as one CSV line, newline included, into `out`.
//
// Returns the number of bytes written, or 0 if `cap` was too small. `out` is
// always null-terminated on success.
//
// NEWLINE IS '\n' AND NOT THE PLATFORM'S. The golden file is committed with LF
// (see .gitattributes) so that a byte-for-byte `diff` between a regenerated file
// and the committed one is meaningful on both platforms. Callers open their
// stream in binary mode for the same reason.
std::size_t format_frame_csv(const AudioFrame& frame, char* out, std::size_t cap);

}  // namespace holocron
