// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// The only translation unit that includes SDL. See sdl_sink.hpp for why the
// header does not.

#include <holocron/sdl_sink.hpp>

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

namespace holocron {

namespace {

// The app-side format is ALWAYS float. RenderCallback is defined in terms of
// interleaved float in [-1, 1], and SDL converts to whatever the device wants.
//
// That conversion is worth naming plainly, because it bears on D-004 and #36:
// audio handed to this sink is NOT bit-perfect on the wire. SDL resamples and
// requantises inside the stream, and nothing here can observe or prevent it.
// The bit-perfect path is WasapiSink's job in exclusive mode. SdlSink is for
// hearing the thing and for CI; it is not the audiophile path and must not be
// mistaken for one.
constexpr SDL_AudioFormat kAppFormat = SDL_AUDIO_F32;

// What the device ended up running at, mapped back onto the interface's enum
// so format() reports the device rather than our fixed app-side float.
SampleFormat from_sdl_format(SDL_AudioFormat f)
{
    switch (f) {
    case SDL_AUDIO_S16LE:
    case SDL_AUDIO_S16BE: return SampleFormat::kInt16;
    case SDL_AUDIO_S32LE:
    case SDL_AUDIO_S32BE: return SampleFormat::kInt32;
    default:              return SampleFormat::kFloat32;
    }
}

// SDL reports failures as a bool plus a thread-local error string. Almost
// nothing it says is machine-classifiable, so most of it lands on
// kBackendFailure rather than pretending to a precision we do not have. The one
// case worth separating is "no device at all", which is what a machine with no
// audio hardware reports and is genuinely a different problem from a device
// that exists and refused.
SinkError classify_open_failure()
{
    const char* msg = SDL_GetError();
    if (msg != nullptr && *msg != '\0') {
        // SDL says "no audio device" / "No such audio device" depending on
        // backend. Substring-matching an error string is unpleasant, and it is
        // done here only to separate a genuinely different recovery from the
        // generic case -- never to drive control flow beyond that.
        if (std::strstr(msg, "no audio device") != nullptr ||
            std::strstr(msg, "No such audio device") != nullptr) {
            return SinkError::kDeviceNotFound;
        }
    }
    return SinkError::kBackendFailure;
}

}  // namespace

// ---------------------------------------------------------------------------

struct SdlSink::Impl {
    SDL_AudioStream* stream = nullptr;

    SinkFormat    fmt{};
    std::uint32_t period   = 0;
    // The rate the CALLBACK is fed at, which is not necessarily the rate the
    // device runs at -- SDL converts between them inside the stream. Both are
    // needed: frames are counted on the app side and clock() must report on the
    // device side, because format().sample_rate is what a caller will divide by.
    std::uint32_t app_rate = 0;

    RenderCallback cb   = nullptr;
    void*          user = nullptr;

    // Preallocated. The callback must never allocate, so the scratch buffer is
    // sized once at open() and never touched again while running.
    std::vector<float> scratch;

    std::atomic<std::uint64_t> frames_submitted{0};
    std::atomic<std::uint64_t> callbacks{0};

    bool open_   = false;
    bool running = false;

    std::uint32_t bytes_per_frame() const
    {
        return static_cast<std::uint32_t>(fmt.channels) * static_cast<std::uint32_t>(sizeof(float));
    }

    // SDL's get-callback. Runs on SDL's audio thread.
    //
    // SDL asks for a VARIABLE byte count; the AudioSink contract promises the
    // user callback an EXACT frame count. Those are reconciled here by always
    // rendering whole period-sized chunks and feeding them until SDL's request
    // is covered. The last chunk may overshoot by less than one period -- that
    // is not waste, SDL simply keeps it queued and asks for correspondingly
    // less next time.
    static void SDLCALL on_audio(void* userdata,
                                 SDL_AudioStream* stream,
                                 int additional_amount,
                                 int /*total_amount*/)
    {
        auto* self = static_cast<Impl*>(userdata);
        if (self == nullptr || additional_amount <= 0) {
            return;
        }

        self->callbacks.fetch_add(1, std::memory_order_relaxed);

        const std::uint32_t bpf         = self->bytes_per_frame();
        const std::uint32_t period      = self->period;
        const int           chunk_bytes = static_cast<int>(period * bpf);
        if (bpf == 0 || period == 0 || chunk_bytes <= 0) {
            return;
        }

        float* const        out      = self->scratch.data();
        const std::uint16_t channels = self->fmt.channels;
        const RenderCallback cb      = self->cb;

        int remaining = additional_amount;
        while (remaining > 0) {
            if (cb != nullptr) {
                cb(out, period, channels, self->user);
            } else {
                // Not reachable through open(), which rejects a null callback.
                // Silence rather than stale scratch, so a future path that
                // clears cb while running cannot repeat the last buffer.
                std::memset(out, 0, static_cast<std::size_t>(chunk_bytes));
            }

            if (!SDL_PutAudioStreamData(stream, out, chunk_bytes)) {
                return;  // device is going away; stop feeding it
            }

            self->frames_submitted.fetch_add(period, std::memory_order_relaxed);
            remaining -= chunk_bytes;
        }
    }
};

// ---------------------------------------------------------------------------

SdlSink::SdlSink() : impl_(std::make_unique<Impl>())
{
    // SDL reference-counts subsystem init, so two sinks are safe and the
    // second one destructing does not take the first one's subsystem with it.
    SDL_InitSubSystem(SDL_INIT_AUDIO);
}

SdlSink::~SdlSink()
{
    close();
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

bool SdlSink::use_dummy_driver()
{
    // The hint is read when the audio subsystem initialises. Once that has
    // happened the choice is made, and silently returning success would let a
    // test believe it was headless while talking to real hardware.
    if (SDL_WasInit(SDL_INIT_AUDIO) != 0) {
        return false;
    }
    return SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
}

const char* SdlSink::current_driver()
{
    return SDL_GetCurrentAudioDriver();
}

SinkError SdlSink::open(const SinkFormat& desired, RenderCallback cb, void* user)
{
    if (impl_->open_) {
        return SinkError::kAlreadyOpen;
    }
    if (cb == nullptr || desired.channels == 0 || desired.sample_rate == 0) {
        return SinkError::kFormatUnsupported;
    }

    SDL_AudioSpec spec{};
    spec.format   = kAppFormat;
    spec.channels = static_cast<int>(desired.channels);
    spec.freq     = static_cast<int>(desired.sample_rate);

    // WHAT THE PLATFORM WOULD HAVE CHOSEN, asked BEFORE opening anything.
    //
    // Diagnostic, and it exists because the question it answers cost a
    // measurement to settle. SDL opens the device at the format the application
    // asks for where it can, so format() afterwards reports the rate SDL
    // negotiated -- NOT what the platform mixer runs at underneath. On Android
    // those differ by design: every mixer output on the Shield is 48 kHz 16-bit
    // (D-053), and an AudioTrack opened at 44.1 kHz is resampled below the frame
    // counter, invisibly.
    //
    // Printing both is what makes that visible from a log rather than from
    // `dumpsys media.audio_flinger`. A run where these two disagree is a run
    // where something below this sink is converting and cannot be asked about
    // it -- which is exactly what is_bit_perfect() reports and why it is false.
    SDL_AudioSpec native{};
    int           native_frames = 0;
    const bool    have_native =
        SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &native, &native_frames);

    SDL_AudioStream* stream =
        SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &Impl::on_audio, impl_.get());
    if (stream == nullptr) {
        return classify_open_failure();
    }

    // What the DEVICE settled on, which is not necessarily what was asked for.
    // period_frames comes from here and is the unit the whole pull model runs
    // on, so a failure to read it is fatal rather than papered over with a
    // guess -- a wrong period silently changes the latency the analysis tap is
    // trimmed against.
    SDL_AudioSpec device_spec{};
    int           device_frames = 0;
    if (!SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream), &device_spec, &device_frames) ||
        device_frames <= 0) {
        SDL_DestroyAudioStream(stream);
        return SinkError::kBackendFailure;
    }

    impl_->stream = stream;
    impl_->cb     = cb;
    impl_->user   = user;
    impl_->period = static_cast<std::uint32_t>(device_frames);

    // Reported format is the device's, except the channel count, which is the
    // app side's: that is what the callback is asked to fill and what the
    // caller must interleave to. Reporting the device's channel count here
    // would tell the caller to produce a layout SDL never asks it for.
    impl_->fmt.sample_rate = static_cast<std::uint32_t>(device_spec.freq);
    impl_->fmt.channels    = desired.channels;
    impl_->fmt.format      = from_sdl_format(device_spec.format);
    impl_->app_rate        = desired.sample_rate;

    // UNCONDITIONAL, because "they agreed" is as informative as "they did not"
    // and the question this answers is one a reader will have.
    //
    // MEASURED ON THE SHIELD, 2026-08-11: these AGREE at 44100, while the
    // platform mixer underneath is running at 48000 (D-053, confirmed in
    // `dumpsys media.audio_flinger`). So SDL cannot tell us the mixer's real
    // rate on Android -- it reports the rate the AudioTrack was opened at, and
    // AudioFlinger resamples below that without saying so.
    //
    // That is why is_bit_perfect() is a flat false here rather than a comparison
    // of these two numbers: the comparison would say "no conversion" and be
    // wrong.
    if (have_native) {
        std::printf("holocron:   device %u Hz, platform default %d Hz%s\n",
                    impl_->fmt.sample_rate, native.freq,
                    static_cast<std::uint32_t>(native.freq) == impl_->fmt.sample_rate
                        ? ""
                        : " -- something below this sink is resampling");
        std::fflush(stdout);
    }

    impl_->scratch.assign(static_cast<std::size_t>(impl_->period) * desired.channels, 0.0f);

    impl_->frames_submitted.store(0, std::memory_order_relaxed);
    impl_->callbacks.store(0, std::memory_order_relaxed);
    impl_->open_   = true;
    impl_->running = false;

    return SinkError::kOk;
}

void SdlSink::close()
{
    if (!impl_->open_) {
        return;  // idempotent, per the interface
    }
    // Destroying the stream also closes the device SDL_OpenAudioDeviceStream
    // opened, and SDL guarantees the callback is not running once this
    // returns.
    SDL_DestroyAudioStream(impl_->stream);
    impl_->stream  = nullptr;
    impl_->cb      = nullptr;
    impl_->user    = nullptr;
    impl_->period  = 0;
    impl_->fmt     = SinkFormat{};
    impl_->open_   = false;
    impl_->running = false;
    impl_->scratch.clear();
}

SinkError SdlSink::start()
{
    if (!impl_->open_) {
        return SinkError::kNotOpen;
    }
    if (impl_->running) {
        return SinkError::kOk;  // idempotent
    }
    if (!SDL_ResumeAudioStreamDevice(impl_->stream)) {
        return SinkError::kBackendFailure;
    }
    impl_->running = true;
    return SinkError::kOk;
}

SinkError SdlSink::stop()
{
    if (!impl_->open_) {
        return SinkError::kNotOpen;
    }
    if (!impl_->running) {
        return SinkError::kOk;
    }
    if (!SDL_PauseAudioStreamDevice(impl_->stream)) {
        return SinkError::kBackendFailure;
    }

    // The interface promises that after stop() returns it is safe to destroy
    // anything the callback touched. Pausing alone does NOT promise that -- a
    // callback already in flight keeps running. Taking the stream lock blocks
    // until it finishes, because SDL holds that same lock across the callback.
    // Without this the obvious caller pattern (stop, then free the buffer the
    // callback was reading) is a use-after-free that only shows up under load.
    if (SDL_LockAudioStream(impl_->stream)) {
        SDL_UnlockAudioStream(impl_->stream);
    }

    impl_->running = false;
    return SinkError::kOk;
}

bool SdlSink::is_open() const    { return impl_->open_; }
bool SdlSink::is_running() const { return impl_->running; }

SinkFormat    SdlSink::format()        const { return impl_->fmt; }
std::uint32_t SdlSink::period_frames() const { return impl_->period; }

SinkClock SdlSink::clock() const
{
    SinkClock c{};
    if (!impl_->open_ || !impl_->running) {
        return c;  // valid stays false; a zeroed pair is not "the beginning"
    }

    const std::uint64_t submitted = impl_->frames_submitted.load(std::memory_order_relaxed);
    if (submitted == 0) {
        return c;  // nothing has been handed over yet, so there is no position
    }

    const int queued_bytes = SDL_GetAudioStreamQueued(impl_->stream);
    const std::uint32_t bpf = impl_->bytes_per_frame();

    std::uint64_t queued_frames = 0;
    if (queued_bytes > 0 && bpf > 0) {
        queued_frames = static_cast<std::uint64_t>(queued_bytes) / bpf;
    }

    // Derived, not measured. See the header: SDL has no hardware clock, so this
    // is "what we handed over, minus what has not been consumed yet". It is
    // monotonic and good enough to drive visuals, and it is the reason
    // WasapiSink exists.
    const std::uint64_t app_played = submitted > queued_frames ? submitted - queued_frames : 0;

    // Reported in DEVICE frames, not app frames, so that dividing by
    // format().sample_rate yields seconds of audio played. Counting happens on
    // the app side because that is what this sink hands over; SDL resamples
    // between the two, so the two counts differ whenever the device is not
    // running at the source's rate.
    //
    // This was wrong until #53 needed the clock for something more demanding
    // than a progress bar. It never showed up before because nothing divided by
    // the rate -- the number was only ever compared against itself.
    if (impl_->app_rate != 0 && impl_->fmt.sample_rate != impl_->app_rate) {
        c.frames_played = (app_played * impl_->fmt.sample_rate) / impl_->app_rate;
    } else {
        c.frames_played = app_played;
    }

    c.timestamp_seconds = static_cast<double>(SDL_GetTicksNS()) / 1'000'000'000.0;
    c.valid             = true;
    return c;
}

const char* SdlSink::backend_name() const { return "sdl3"; }

bool SdlSink::is_bit_perfect() const
{
    // Not "no path here computes it yet" -- there is nothing to compute. Every
    // route through this sink crosses a shared platform mixer, and a mixer that
    // resamples below the handle cannot be asked what it did. See the header.
    return false;
}

const char* SdlSink::bit_perfect_note() const
{
#ifdef __ANDROID__
    // D-053. Read out of the device's own audio policy rather than assumed:
    // every mixer output on the Shield is 48 kHz 16-bit, and every output that
    // carries 44.1 kHz is AUDIO_OUTPUT_FLAG_DIRECT, which the NDK does not
    // expose. So this is a property of the hardware and not of the code, and no
    // amount of work on this sink changes it.
    return "every Android mixer output is 48 kHz 16-bit and the NDK exposes no direct path";
#else
    return "SDL goes through the shared system mixer";
#endif
}

std::uint64_t SdlSink::callbacks_served() const
{
    return impl_->callbacks.load(std::memory_order_relaxed);
}

}  // namespace holocron
