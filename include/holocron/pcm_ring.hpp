// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/pcm_ring.hpp
//
// The lock-free PCM handoff between the decode thread and the audio callback.
//
// WHY THIS IS NOT A TripleBuffer
//
// They solve opposite problems and both are needed.
//
// TripleBuffer publishes the NEWEST value and drops everything else, because
// the render thread wants the current state of the world and skipping is
// correct. Audio is the exact opposite: every sample must be delivered, in
// order, exactly once. A dropped buffer is not a skipped frame, it is a click.
//
// So this is a ring: bounded, ordered, lossless within its capacity.
//
// WHY NOT A MUTEX AND A DEQUE
//
// Same reason as TripleBuffer. The consumer is a real-time audio callback,
// bound by audio_sink.hpp's contract: no allocation, no locks, no blocking. A
// mutex here would be a priority inversion waiting for a scheduler hiccup.
//
// THREADING CONTRACT -- SINGLE PRODUCER, SINGLE CONSUMER
//
// Exactly one thread may call write(); exactly one different thread may call
// read(). reset() may only be called when neither is running. This is not an
// MPMC structure and is not safe as one.
//
// DEPTH IS A REAL TRADE, NOT A DEFAULT
//
// Every frame of depth here is a frame by which the visuals lead the sound,
// because the analysis runs on audio as it is decoded rather than as it is
// played. Too shallow and the ring runs dry on any scheduling hiccup -- on
// Windows the default timer resolution is ~15.6 ms, so a "1 ms" sleep in the
// producer really costs about fifteen. Too deep and the picture drifts away
// from the music. The caller picks, and silence_padded() tells it whether it
// picked wrong.

#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace holocron {

class PcmRing {
public:
    // Not safe to call while a producer or consumer is running.
    void reset(std::size_t frames, std::uint16_t channels)
    {
        channels_ = channels == 0 ? 1 : channels;
        capacity_ = frames;
        buf_.assign(capacity_ * channels_, 0.0f);
        read_.store(0, std::memory_order_relaxed);
        write_.store(0, std::memory_order_relaxed);
        silence_padded_.store(0, std::memory_order_relaxed);
    }

    std::size_t   capacity() const { return capacity_; }
    std::uint16_t channels() const { return channels_; }

    // One slot is left permanently empty so that "full" and "empty" are
    // distinguishable without a separate count that both threads would have to
    // agree on.
    std::size_t writable() const
    {
        if (capacity_ == 0) {
            return 0;
        }
        const std::size_t w = write_.load(std::memory_order_relaxed);
        const std::size_t r = read_.load(std::memory_order_acquire);
        return capacity_ - (w - r) - 1;
    }

    std::size_t readable() const
    {
        const std::size_t w = write_.load(std::memory_order_acquire);
        const std::size_t r = read_.load(std::memory_order_relaxed);
        return w - r;
    }

    // -- producer -----------------------------------------------------------
    //
    // Writes as much as fits and reports how much that was. A short write is
    // normal back-pressure, not an error: it means the consumer has not caught
    // up and the producer should wait rather than overwrite unplayed audio.
    std::size_t write(const float* src, std::size_t frames)
    {
        if (capacity_ == 0 || src == nullptr) {
            return 0;
        }
        const std::size_t n = std::min(frames, writable());
        const std::size_t w = write_.load(std::memory_order_relaxed);

        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t slot = ((w + i) % capacity_) * channels_;
            for (std::uint16_t c = 0; c < channels_; ++c) {
                buf_[slot + c] = src[(i * channels_) + c];
            }
        }

        // Release: every sample written above must be visible to the consumer
        // before the index that exposes them.
        write_.store(w + n, std::memory_order_release);
        return n;
    }

    // -- consumer -----------------------------------------------------------
    //
    // ALWAYS fills `frames`, padding with silence when the ring is short.
    // Returning short is not an option for a render callback -- audio_sink.hpp
    // is explicit that producing fewer frames underruns the device -- and
    // silence is the correct thing to play when there is nothing to play.
    //
    // Returns the number of REAL frames delivered, so short returns remain
    // visible to a caller that cares.
    std::size_t read(float* dst, std::size_t frames)
    {
        if (dst == nullptr) {
            return 0;
        }
        const std::size_t n = std::min(frames, readable());
        const std::size_t r = read_.load(std::memory_order_relaxed);

        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t slot = ((r + i) % capacity_) * channels_;
            for (std::uint16_t c = 0; c < channels_; ++c) {
                dst[(i * channels_) + c] = buf_[slot + c];
            }
        }
        for (std::size_t i = n; i < frames; ++i) {
            for (std::uint16_t c = 0; c < channels_; ++c) {
                dst[(i * channels_) + c] = 0.0f;
            }
        }

        if (n < frames) {
            silence_padded_.fetch_add(frames - n, std::memory_order_relaxed);
        }

        read_.store(r + n, std::memory_order_release);
        return n;
    }

    // Frames of silence delivered because the ring was empty. THIS is the real
    // underrun measurement for the whole audio path.
    //
    // It lives here rather than in the sink because this is the only place that
    // knows the answer. SdlSink always satisfies whatever SDL asks for, so from
    // inside it every callback looks healthy -- see the note in sdl_sink.hpp
    // about two earlier metrics that were removed for saying nothing. Every
    // frame counted here is a frame of silence that was played instead of
    // music.
    std::uint64_t silence_padded() const
    {
        return silence_padded_.load(std::memory_order_relaxed);
    }

private:
    std::vector<float> buf_;
    std::size_t        capacity_ = 0;
    std::uint16_t      channels_ = 2;

    std::atomic<std::size_t>   read_{0};
    std::atomic<std::size_t>   write_{0};
    std::atomic<std::uint64_t> silence_padded_{0};
};

}  // namespace holocron
