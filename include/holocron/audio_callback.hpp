// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/audio_callback.hpp
//
// The body of the audio callback: everything that runs on the device thread
// between one wakeup and the next.
//
// WHY IT IS HERE AND NOT WHERE IT IS INSTALLED.
//
// It used to be four lines inside an anonymous namespace in
// src/audio/playback_session.cpp, which is the natural place for it and made
// M1's eighth exit criterion -- "zero allocation and zero locks in the audio
// callback, DEMONSTRATED not assumed" -- impossible to meet without either a
// test seam or a second copy of the code. A test that reimplements the callback
// and proves the reimplementation allocates nothing has demonstrated nothing.
//
// So the policy lives here, `render_audio` in playback_session.cpp is a
// three-line adapter that unpacks its `void*`, and tests/test_audio_callback.cpp
// calls THIS. One body, one place the audio-path rule has to hold, and a test
// that is looking at the real thing.
//
// -- what "zero allocation and zero locks" costs, concretely -------------------
//
// The device thread runs at real-time priority and has one period to fill a
// buffer -- 160 frames at 48 kHz on the rack, so 3.3 ms. `malloc` can take a
// lock held by an ordinary-priority thread that has been preempted, and a
// real-time thread waiting on it is a priority inversion that shows up as a
// click. It does not show up in a test, on a desk, or in any measurement short
// of listening to the room.
//
// That is why the rule is stated as absolute rather than as a budget: there is
// no small number of allocations that is safe, because the cost is not the
// allocation, it is the tail.

#pragma once

#include <holocron/pcm_ring.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace holocron {

// Fill `out` with `frames` frames from `pcm`, padding with silence if the ring
// has run dry.
//
// `decoder_finished` and `drain_padded` exist to tell TWO DIFFERENT EVENTS
// APART. A ring that runs dry mid-track is an underrun and a real fault; a ring
// that runs dry after the decoder has finished is the file ending, which happens
// on every complete play. One counter for both cries wolf on every track, and a
// metric that is always red is a metric nobody reads. So the frames padded after
// the decoder finished are counted separately and subtracted from the total the
// player reports.
//
// EVERY LINE OF THIS IS ON THE AUDIO PATH. Two relaxed atomic loads, the ring
// read, and at most one relaxed fetch_add -- no allocation, no lock, no syscall,
// no branch on anything a lock protects. tests/test_audio_callback.cpp counts
// the allocations rather than trusting this paragraph, by replacing the global
// operator new; it was confirmed on 2026-08-10 to fail on a single `new int`
// added here, in all three of its cases including the one running on a real
// device thread.
inline void render_from_ring(PcmRing& pcm, float* out, std::size_t frames,
                             bool decoder_finished,
                             std::atomic<std::uint64_t>& drain_padded)
{
    const std::uint64_t before = pcm.silence_padded();

    pcm.read(out, frames);

    if (decoder_finished) {
        const std::uint64_t after = pcm.silence_padded();
        if (after > before) {
            drain_padded.fetch_add(after - before, std::memory_order_relaxed);
        }
    }
}

}  // namespace holocron
