// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// See playback_session.hpp.
//
// Everything here was lifted out of tools/player/main.cpp, where it had grown
// around the assumption of one file that never changes. The comments that
// explain WHY a number is what it is came with it -- the ring depth and the
// prefill in particular were both measured rather than chosen, and losing that
// reasoning in a move would invite it being "tidied" back to a guess.

#include <holocron/playback_session.hpp>

#include <holocron/analysis.hpp>
#include <holocron/decoder.hpp>
#include <holocron/pcm_ring.hpp>
#include <holocron/sdl_sink.hpp>
#include <holocron/wasapi_sink.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

namespace holocron {

const char* to_string(SessionError e)
{
    switch (e) {
    case SessionError::kOk:               return "ok";
    case SessionError::kCannotOpenSource: return "the track could not be opened";
    case SessionError::kNoAudioDevice:    return "no audio device could be opened";
    }
    return "unknown";
}

// What the decode thread and the audio callback share. Nothing here is touched
// by the renderer except through the accessors on PlaybackSession.
struct Shared {
    FrameHistory<AudioFrame, kHistorySlots> frames;
    PcmRing                                 pcm;

    std::atomic<bool> quit{false};
    std::atomic<bool> finished{false};

    // Frames padded AFTER the decoder finished, which is the file ending rather
    // than a fault. Subtracted from the ring's total so the reported underrun
    // count means only what it claims to.
    std::atomic<std::uint64_t> drain_padded{0};
    std::atomic<std::uint64_t> published{0};
};

namespace {

void on_analysis_frame(const AudioFrame& f, void* user)
{
    auto* s = static_cast<Shared*>(user);

    const std::uint64_t k = s->published.fetch_add(1, std::memory_order_relaxed);

    // The instant this frame REPRESENTS, which is the centre of its analysis
    // window rather than its start. Keying on the window's start would place
    // every frame kFftSize/2 too early -- about 21 ms, a fifth of the beat at
    // 120 BPM, and plainly visible as the visuals running ahead.
    const std::uint64_t centre_samples =
        (k * static_cast<std::uint64_t>(kHopSize)) + static_cast<std::uint64_t>(kFftSize) / 2;
    const std::uint64_t position_us =
        (centre_samples * 1'000'000ULL) / static_cast<std::uint64_t>(kAnalysisRate);

    s->frames.publish(f, position_us);
}

void render_audio(float* out, std::size_t frames, std::uint16_t channels, void* user)
{
    auto* s = static_cast<Shared*>(user);
    (void)channels;

    // A ring that runs dry MID-TRACK and one that runs dry at the END OF THE
    // FILE are not the same event, and one counter for both cries wolf. After
    // the decoder has finished, a handful of padded periods is simply the file
    // ending, and that happens on every complete play.
    //
    // Two relaxed atomic loads and no allocation, so the audio-path rule holds.
    const bool          done   = s->finished.load(std::memory_order_acquire);
    const std::uint64_t before = s->pcm.silence_padded();

    s->pcm.read(out, frames);

    if (done) {
        const std::uint64_t after = s->pcm.silence_padded();
        if (after > before) {
            s->drain_padded.fetch_add(after - before, std::memory_order_relaxed);
        }
    }
}

}  // namespace

struct PlaybackSession::Impl {
    SessionConfig config;

    // REPLACED WHOLESALE ON EVERY start(), rather than reset in place.
    //
    // FrameHistory has no clear(), and adding one to a lock-free structure that
    // is already tested against real thread contention -- for the sake of a
    // path that only ever runs while nothing is reading -- is a poor trade. A
    // fresh object is unambiguously empty.
    //
    // It is safe because start() calls stop() first: the decode thread is
    // joined and the device is closed before this is replaced, so nothing holds
    // a pointer to the old one.
    //
    // Heap-allocated regardless. 1.38 MB of FrameHistory on a 1 MB thread stack
    // exits with 0xC00000FD and no output at all.
    std::unique_ptr<Shared> shared = std::make_unique<Shared>();

    std::unique_ptr<AudioSink> sink;
    std::thread                decoder;

    bool       started       = false;   // start() succeeded and stop() has not run
    bool       audio_started = false;   // device opened
    bool       audio_running = false;   // device opened AND pulling
    bool       bit_perfect   = false;
    NowPlaying what;

    std::uint32_t rate         = 0;
    double        lead_budget  = 0.0;

    void decode_loop(std::string source, std::int64_t offset_ms, bool feed_audio);
};

void PlaybackSession::Impl::decode_loop(std::string source, std::int64_t offset_ms,
                                        bool feed_audio)
{
    Decoder decoder_local;
    if (decoder_local.open(source.c_str()) != DecoderError::kOk) {
        shared->finished.store(true, std::memory_order_release);
        return;
    }

    const SourceInfo info = decoder_local.info();

    Resampler resampler;
    resampler.configure(info.sample_rate, info.channels);

    AnalysisStage analysis;
    analysis.set_source_sample_rate(info.sample_rate);

    constexpr std::size_t kChunk = 1024;
    std::vector<float>    native(kChunk * info.channels);
    std::vector<float>    tapped(resampler.max_output_frames(kChunk) * 2 + 64);

    std::uint64_t decoded_frames = 0;

    // REACHING AN OFFSET BY DECODING AND THROWING IT AWAY.
    //
    // `Decoder` has no seek, and adding one means av_seek_frame plus deciding
    // what to do about a stream that cannot seek -- a contained change, but not
    // this one. Discarding is CORRECT for every source, including a remote URL,
    // and for the case that actually happens -- Plexamp resuming a few seconds
    // in -- it costs nothing measurable.
    //
    // It is genuinely slow for a large offset on a remote track, because the
    // prefix really is downloaded. Tracked separately rather than left as a
    // surprise; the honest version is here and the fast version is an issue.
    std::uint64_t discard_frames =
        offset_ms > 0 ? static_cast<std::uint64_t>(offset_ms) *
                            static_cast<std::uint64_t>(info.sample_rate) / 1000ULL
                      : 0;

    while (!shared->quit.load(std::memory_order_relaxed)) {
        // Back-pressure. Without this the decoder races ahead of playback and
        // the ring stays permanently full, so the visuals would lead by the
        // whole file rather than by the ring depth.
        //
        // One millisecond, and it still is not a real wait: Windows' default
        // timer resolution is ~15.6 ms, so ANY short sleep here is really a
        // ~15 ms sleep. That is why the ring is sized in time below.
        if (feed_audio && discard_frames == 0 && shared->pcm.writable() < kChunk) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        const std::size_t got = decoder_local.read(native.data(), kChunk);
        if (got == 0) {
            break;
        }

        if (discard_frames > 0) {
            // Still winding forward. Nothing is published and nothing is fed to
            // the device, so the analysis and the audio both begin at the point
            // the caller asked for rather than at the top of the track.
            const std::uint64_t skipped = std::min<std::uint64_t>(discard_frames, got);
            discard_frames -= skipped;
            decoded_frames += skipped;
            if (skipped == got) {
                continue;
            }
        }

        decoded_frames += got;
        analysis.set_track_position(
            static_cast<double>(decoded_frames) / static_cast<double>(info.sample_rate),
            info.duration_seconds);

        if (feed_audio) {
            std::size_t written = 0;
            while (written < got && !shared->quit.load(std::memory_order_relaxed)) {
                written += shared->pcm.write(native.data() + (written * info.channels),
                                            got - written);
                if (written < got) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
        }

        const std::size_t tap =
            resampler.process(native.data(), got, tapped.data(), tapped.size() / 2);
        if (tap > 0) {
            analysis.push(tapped.data(), tap, 2, &on_analysis_frame, shared.get());
        }

        // With no audio device there is nothing pacing the decode, so it would
        // run the whole file in a fraction of a second and the window would show
        // only the end. Pace it to roughly real time instead.
        if (!feed_audio) {
            const auto ms = static_cast<long long>(
                (static_cast<double>(got) / static_cast<double>(info.sample_rate)) * 1000.0);
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        }
    }

    shared->finished.store(true, std::memory_order_release);
}

PlaybackSession::PlaybackSession(const SessionConfig& config) : impl_(std::make_unique<Impl>())
{
    impl_->config = config;
}

PlaybackSession::~PlaybackSession()
{
    stop();
}

SessionError PlaybackSession::start(const std::string& source, std::int64_t offset_ms,
                                    const NowPlaying& what, std::string& out_detail)
{
    out_detail.clear();

    // REPLACES. Stopping first is what makes a cast of a second track work at
    // all, and doing it here rather than asking the caller to remember is what
    // stops two decode threads racing for one ring.
    stop();

    Impl& impl = *impl_;

    // A FRESH Shared, rather than resetting the old one in place.
    //
    // Only safe because stop() above has joined the decode thread and closed the
    // device, so nothing holds a pointer to the outgoing one. The alternative --
    // adding clear() to FrameHistory -- means putting a not-concurrency-safe
    // method on a lock-free structure that is otherwise tested against real
    // contention, for a path that only ever runs while nothing is reading.
    impl.shared = std::make_unique<Shared>();

    // Probe on the caller's thread. The format the device is asked for follows
    // the SOURCE, so this has to happen before the sink is opened -- and a
    // source that will not open should fail here, where the caller can report
    // it, rather than on a thread that has already been spawned.
    SourceInfo info{};
    {
        Decoder probe;
        if (probe.open(source.c_str()) != DecoderError::kOk) {
            out_detail = "cannot open the track";
            return SessionError::kCannotOpenSource;
        }
        info = probe.info();
        probe.close();
    }

    impl.what = what;
    impl.rate = info.sample_rate;

    if (!impl.config.no_audio) {
        SinkFormat want;
        want.sample_rate = info.sample_rate;
        want.channels    = info.channels;
        // Ask for the source's own depth. Exclusive mode negotiates DEPTH but
        // never RATE -- that is #32.
        want.format = info.is_lossless ? SampleFormat::kInt24 : SampleFormat::kFloat32;

        SinkError err = SinkError::kBackendFailure;

        // Preference order, and the reasoning is in the order itself:
        //   1. WASAPI exclusive -- the only bit-perfect path (D-004)
        //   2. WASAPI shared    -- not bit-perfect, but still a real device clock
        //   3. SDL              -- portable fallback, derived clock
        //
        // The fallback happens HERE, in the caller of the sink, not inside it.
        // Per #32 a sink that quietly retries somewhere else has broken a
        // promise nobody can detect.
        if (impl.config.backend != SessionConfig::Backend::kSdl && WasapiSink::available()) {
            auto exclusive = std::make_unique<WasapiSink>();
            exclusive->set_mode(WasapiMode::kExclusive);
            err = exclusive->open(want, &render_audio, impl.shared.get());

            if (err == SinkError::kOk) {
                impl.bit_perfect = exclusive->is_bit_perfect();
                impl.sink        = std::move(exclusive);
            } else {
                out_detail = to_string(err);

                auto shared_mode = std::make_unique<WasapiSink>();
                shared_mode->set_mode(WasapiMode::kShared);
                if (shared_mode->open(want, &render_audio, impl.shared.get()) == SinkError::kOk) {
                    impl.bit_perfect = shared_mode->is_bit_perfect();
                    impl.sink        = std::move(shared_mode);
                }
            }
        }

        if (impl.sink == nullptr && impl.config.backend != SessionConfig::Backend::kWasapi) {
            auto sdl = std::make_unique<SdlSink>();
            if (sdl->open(want, &render_audio, impl.shared.get()) == SinkError::kOk) {
                impl.sink = std::move(sdl);
            }
        }

        if (impl.sink == nullptr) {
            // NOT fatal. A machine with no usable device should still draw --
            // the visuals are the point, and the analysis runs regardless.
            if (out_detail.empty()) {
                out_detail = to_string(err);
            }
        } else {
            // Ring depth is a MEASURED number, not a guess.
            //
            // Four periods first, on the reasoning that shallow keeps the visual
            // lead small. That reported 12,348 frames of silence padded over
            // seven seconds -- audible, and invisible until the ring counted it.
            // The cause is Windows' ~15.6 ms timer resolution against a ~10 ms
            // period: one late wake-up empties a 40 ms ring.
            //
            // Sixteen periods is roughly ten times the worst-case sleep and is
            // the FLOOR below which it starves. But sixteen periods is ALSO the
            // lead budget, and that turned out to be the binding constraint --
            // at 160-frame exclusive periods it is only ~58 ms, which is why a
            // trim below that did nothing at all.
            //
            // So the ring is sized by TIME. Periods are the wrong unit:
            // exclusive gives 160 frames and shared gives ~441, so the same
            // multiplier bought wildly different amounts of lead.
            const std::size_t floor_frames =
                static_cast<std::size_t>(impl.sink->period_frames()) * 16;
            const std::size_t lead_frames = static_cast<std::size_t>(
                impl.config.lead_ms * static_cast<double>(info.sample_rate) / 1000.0);

            impl.shared->pcm.reset(std::max(floor_frames, lead_frames), info.channels);
            impl.lead_budget = 1000.0 * static_cast<double>(impl.shared->pcm.capacity()) /
                               static_cast<double>(info.sample_rate);
            impl.audio_started = true;
        }
    }

    impl.decoder = std::thread(&Impl::decode_loop, &impl, source, offset_ms, impl.audio_started);
    impl.started = true;

    // Prefill before the device starts pulling.
    //
    // Starting the sink first means its opening callbacks arrive at an empty
    // ring and are answered with silence -- a guaranteed dropout at the top of
    // every track, which is exactly where it is most noticeable. The deadline is
    // a safety net for a source that decodes to less than one ring, not the
    // expected path.
    if (impl.audio_started) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline &&
               impl.shared->pcm.readable() < impl.shared->pcm.capacity() / 2 &&
               !impl.shared->finished.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        impl.audio_running = impl.sink->start() == SinkError::kOk;
    }

    return SessionError::kOk;
}

void PlaybackSession::stop()
{
    Impl& impl = *impl_;
    if (!impl.started) {
        return;
    }

    impl.shared->quit.store(true, std::memory_order_release);
    if (impl.decoder.joinable()) {
        impl.decoder.join();
    }

    // After the thread is joined, so nothing is still writing into a ring whose
    // device is going away.
    if (impl.sink != nullptr) {
        impl.sink->stop();
        impl.sink->close();
        impl.sink.reset();
    }

    impl.started       = false;
    impl.audio_started = false;
    impl.audio_running = false;
    impl.bit_perfect   = false;
    impl.rate          = 0;
    impl.lead_budget   = 0.0;
    impl.what          = NowPlaying{};
}

bool PlaybackSession::active() const { return impl_->started; }

bool PlaybackSession::finished() const
{
    return impl_->shared->finished.load(std::memory_order_acquire);
}

bool PlaybackSession::audio_running() const { return impl_->audio_running; }

std::size_t PlaybackSession::pending_frames() const { return impl_->shared->pcm.readable(); }

const NowPlaying& PlaybackSession::now_playing() const { return impl_->what; }

bool PlaybackSession::played_us(std::uint64_t& out) const
{
    out = 0;
    if (!impl_->audio_running || impl_->sink == nullptr || impl_->rate == 0) {
        return false;
    }
    const SinkClock clock = impl_->sink->clock();
    if (!clock.valid) {
        return false;
    }
    out = (clock.frames_played * 1'000'000ULL) / impl_->rate;
    return true;
}

void PlaybackSession::select_frame(std::uint64_t target_us, AudioFrame& out) const
{
    impl_->shared->frames.select(target_us, out);
}

bool PlaybackSession::newest_frame(AudioFrame& out) const
{
    return impl_->shared->frames.newest(out);
}

std::uint64_t PlaybackSession::newest_position_us() const
{
    const std::uint64_t n = impl_->shared->published.load(std::memory_order_relaxed);
    if (n == 0) {
        return 0;
    }
    return ((((n - 1) * static_cast<std::uint64_t>(kHopSize)) +
             (static_cast<std::uint64_t>(kFftSize) / 2)) *
            1'000'000ULL) /
           static_cast<std::uint64_t>(kAnalysisRate);
}

const char* PlaybackSession::backend_name() const
{
    return impl_->sink != nullptr ? impl_->sink->backend_name() : "(none)";
}

std::uint32_t PlaybackSession::period_frames() const
{
    return impl_->sink != nullptr ? impl_->sink->period_frames() : 0u;
}

std::uint32_t PlaybackSession::sample_rate() const { return impl_->rate; }

bool PlaybackSession::bit_perfect() const { return impl_->bit_perfect; }

double PlaybackSession::lead_budget_ms() const { return impl_->lead_budget; }

std::uint64_t PlaybackSession::frames_published() const
{
    return impl_->shared->published.load(std::memory_order_relaxed);
}

std::uint64_t PlaybackSession::silence_padded_mid_track() const
{
    // The underrun count comes from the RING, not the sink -- the ring is the
    // only thing that knows whether it had audio when it was asked. Two
    // sink-side metrics were deleted for pretending otherwise.
    const std::uint64_t padded = impl_->shared->pcm.silence_padded();
    const std::uint64_t drain  = impl_->shared->drain_padded.load(std::memory_order_relaxed);
    return padded > drain ? padded - drain : 0;
}

std::uint64_t PlaybackSession::silence_padded_draining() const
{
    return impl_->shared->drain_padded.load(std::memory_order_relaxed);
}

std::uint64_t PlaybackSession::device_frames_played() const
{
    if (impl_->sink == nullptr) {
        return 0;
    }
    const SinkClock clock = impl_->sink->clock();
    return clock.valid ? clock.frames_played : 0;
}

}  // namespace holocron
