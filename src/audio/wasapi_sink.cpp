// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// The only translation unit that includes the Windows audio headers.
//
// The whole file is behind _WIN32. On other platforms WasapiSink still exists
// as a type and every call fails cleanly, so callers need no preprocessor
// conditionals and the Linux job still compiles this file -- which is the point:
// a backend excluded from the build is a backend nobody notices breaking.

#include <holocron/wasapi_sink.hpp>

#include <holocron/sample_convert.hpp>

#include <atomic>
#include <cstring>
#include <vector>

#ifdef _WIN32

// NOMINMAX before anything Windows: <windows.h> otherwise defines min and max
// as macros and every std::min in this translation unit stops compiling. Not
// hypothetical -- it is the first thing that breaks.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <avrt.h>
#include <thread>

#endif  // _WIN32

namespace holocron {

#ifdef _WIN32

namespace {

// COM interface ids, referenced rather than linked, so this translation unit
// does not need the uuid libraries on every toolchain.
const CLSID kMMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
const IID   kIMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);
const IID   kIAudioClient        = __uuidof(IAudioClient);
const IID   kIAudioRenderClient  = __uuidof(IAudioRenderClient);
const IID   kIAudioClock         = __uuidof(IAudioClock);

// WASAPI reports failure as an HRESULT and several of them mean genuinely
// different things to a caller. This is the whole reason SinkError is an enum
// and not a bool -- see the note in audio_sink.hpp, which named these four
// before any backend existed to produce them.
SinkError classify(HRESULT hr)
{
    switch (hr) {
    case AUDCLNT_E_DEVICE_IN_USE:
        return SinkError::kDeviceBusy;
    case AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED:
        // Policy, not capability. The user can fix this with a checkbox, which
        // is a completely different message from "your hardware cannot do it".
        return SinkError::kExclusiveModeNotPermitted;
    case AUDCLNT_E_UNSUPPORTED_FORMAT:
        return SinkError::kFormatUnsupported;
    case AUDCLNT_E_ENDPOINT_CREATE_FAILED:
        return SinkError::kDeviceNotFound;
    default:
        return SinkError::kBackendFailure;
    }
}

int bits_for(SampleFormat f)
{
    switch (f) {
    case SampleFormat::kInt16:   return 16;
    case SampleFormat::kInt24:   return 24;
    case SampleFormat::kInt32:   return 32;
    case SampleFormat::kFloat32: return 32;
    }
    return 16;
}

// Build the WAVEFORMATEXTENSIBLE that describes exactly what we want the wire
// to carry. EXTENSIBLE rather than plain WAVEFORMATEX because 24-in-3-bytes and
// multichannel layouts cannot be described without it, and exclusive mode is
// where those actually come up.
void fill_format(WAVEFORMATEXTENSIBLE& wf, std::uint32_t rate, std::uint16_t channels,
                 SampleFormat fmt)
{
    const bool is_float = (fmt == SampleFormat::kFloat32);
    const WORD bits     = static_cast<WORD>(bits_for(fmt));
    // 24-bit rides in 3 bytes on the wire, not 4. Getting this wrong produces a
    // stream that opens successfully and plays noise.
    const WORD container_bits = (fmt == SampleFormat::kInt24) ? 24 : bits;
    const WORD block          = static_cast<WORD>(channels * (container_bits / 8));

    std::memset(&wf, 0, sizeof(wf));
    wf.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
    wf.Format.nChannels       = channels;
    wf.Format.nSamplesPerSec  = rate;
    wf.Format.wBitsPerSample  = container_bits;
    wf.Format.nBlockAlign     = block;
    wf.Format.nAvgBytesPerSec = rate * block;
    wf.Format.cbSize          = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);

    wf.Samples.wValidBitsPerSample = bits;
    wf.dwChannelMask = (channels == 2) ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : 0;
    wf.SubFormat     = is_float ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
                                : KSDATAFORMAT_SUBTYPE_PCM;
}

}  // namespace

// ---------------------------------------------------------------------------

struct WasapiSink::Impl {
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice*           device     = nullptr;
    IAudioClient*        client     = nullptr;
    IAudioRenderClient*  render     = nullptr;
    IAudioClock*         clock      = nullptr;

    HANDLE      event  = nullptr;
    std::thread worker;

    WasapiMode mode = WasapiMode::kExclusive;

    SinkFormat    fmt{};
    SampleFormat  wire = SampleFormat::kFloat32;
    std::uint32_t period = 0;
    UINT64        clock_freq = 0;

    RenderCallback cb   = nullptr;
    void*          user = nullptr;

    std::vector<float> scratch;   // preallocated: the render loop never allocates

    std::atomic<bool>          running{false};
    std::atomic<std::uint64_t> late{0};

    bool com_initialised = false;
    bool open_           = false;
    bool bit_perfect     = false;

    void release()
    {
        if (clock  != nullptr) { clock->Release();  clock  = nullptr; }
        if (render != nullptr) { render->Release(); render = nullptr; }
        if (client != nullptr) { client->Release(); client = nullptr; }
        if (device != nullptr) { device->Release(); device = nullptr; }
        if (enumerator != nullptr) { enumerator->Release(); enumerator = nullptr; }
        if (event != nullptr) { CloseHandle(event); event = nullptr; }
    }

    // Convert one period of interleaved float into the device's wire format.
    void convert(const float* src, BYTE* dst, std::uint32_t frames) const
    {
        const std::size_t n = static_cast<std::size_t>(frames) * fmt.channels;

        switch (wire) {
        case SampleFormat::kFloat32: {
            std::memcpy(dst, src, n * sizeof(float));
            break;
        }
        case SampleFormat::kInt16: {
            auto* out = reinterpret_cast<std::int16_t*>(dst);
            for (std::size_t i = 0; i < n; ++i) {
                out[i] = static_cast<std::int16_t>(from_float(src[i], 16));
            }
            break;
        }
        case SampleFormat::kInt24: {
            auto* out = reinterpret_cast<unsigned char*>(dst);
            for (std::size_t i = 0; i < n; ++i) {
                write_int24(out + (i * 3), from_float(src[i], 24));
            }
            break;
        }
        case SampleFormat::kInt32: {
            auto* out = reinterpret_cast<std::int32_t*>(dst);
            for (std::size_t i = 0; i < n; ++i) {
                out[i] = from_float(src[i], 32);
            }
            break;
        }
        }
    }

    // The render loop. Owns its own thread and does nothing else, because the
    // event wait must not share a thread with anything that could delay it.
    void run()
    {
        // Ask the scheduler to treat this as pro audio. Without it a busy
        // desktop will make the loop miss period deadlines, and a missed
        // deadline in exclusive mode is an audible gap rather than a slow frame.
        DWORD  task_index = 0;
        HANDLE mm         = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);

        while (running.load(std::memory_order_acquire)) {
            const DWORD wait = WaitForSingleObject(event, 200);
            if (wait == WAIT_TIMEOUT) {
                // The device stopped signalling. Not fatal on its own -- a
                // format change or a device switch does this -- but it means a
                // period went unserviced.
                late.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            if (wait != WAIT_OBJECT_0) {
                break;
            }
            if (!running.load(std::memory_order_acquire)) {
                break;
            }

            UINT32 padding = 0;
            if (mode == WasapiMode::kShared) {
                if (FAILED(client->GetCurrentPadding(&padding))) {
                    break;
                }
            }

            // Exclusive mode is exactly one full period per wakeup. Shared mode
            // asks for whatever is free. audio_sink.hpp promises the callback a
            // fixed frame count, so shared mode is fed in whole periods too and
            // simply skips a wakeup when less than a period is free.
            const UINT32 want = (mode == WasapiMode::kExclusive)
                                    ? period
                                    : (period > padding ? period - padding : 0u);
            if (want < period) {
                continue;
            }

            BYTE* buffer = nullptr;
            if (FAILED(render->GetBuffer(period, &buffer))) {
                late.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            if (cb != nullptr) {
                cb(scratch.data(), period, fmt.channels, user);
            } else {
                std::memset(scratch.data(), 0, scratch.size() * sizeof(float));
            }

            convert(scratch.data(), buffer, period);
            render->ReleaseBuffer(period, 0);
        }

        if (mm != nullptr) {
            AvRevertMmThreadCharacteristics(mm);
        }
    }
};

// ---------------------------------------------------------------------------

WasapiSink::WasapiSink() : impl_(std::make_unique<Impl>()) {}

WasapiSink::~WasapiSink()
{
    close();
}

bool WasapiSink::available() { return true; }

void       WasapiSink::set_mode(WasapiMode m) { impl_->mode = m; }
WasapiMode WasapiSink::mode() const           { return impl_->mode; }
bool WasapiSink::is_bit_perfect() const { return impl_->bit_perfect; }

const char* WasapiSink::bit_perfect_note() const
{
    // Exclusive mode is the only path that can be, so failing to get it IS the
    // reason -- and it is the actionable one: the two checkboxes under Sound ->
    // Playback -> Properties -> Advanced are what decide it. The player already
    // prints which box to tick when open() is refused; this is the same fact
    // said on a run that succeeded in shared mode instead.
    return impl_->mode == WasapiMode::kExclusive
               ? "the device would not take the source format exactly"
               : "shared mode: Windows mixes and resamples";
}

SinkError WasapiSink::open(const SinkFormat& desired, RenderCallback cb, void* user)
{
    if (impl_->open_) {
        return SinkError::kAlreadyOpen;
    }
    if (cb == nullptr || desired.channels == 0 || desired.sample_rate == 0) {
        return SinkError::kFormatUnsupported;
    }

    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // RPC_E_CHANGED_MODE means COM is already up in another mode, which is fine
    // -- it is someone else's initialisation and must not be torn down here.
    impl_->com_initialised = SUCCEEDED(co);

    HRESULT hr = CoCreateInstance(kMMDeviceEnumerator, nullptr, CLSCTX_ALL, kIMMDeviceEnumerator,
                                  reinterpret_cast<void**>(&impl_->enumerator));
    if (FAILED(hr)) {
        impl_->release();
        return SinkError::kBackendFailure;
    }

    hr = impl_->enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &impl_->device);
    if (FAILED(hr)) {
        impl_->release();
        return SinkError::kDeviceNotFound;
    }

    hr = impl_->device->Activate(kIAudioClient, CLSCTX_ALL, nullptr,
                                 reinterpret_cast<void**>(&impl_->client));
    if (FAILED(hr)) {
        impl_->release();
        return SinkError::kBackendFailure;
    }

    const AUDCLNT_SHAREMODE share = (impl_->mode == WasapiMode::kExclusive)
                                        ? AUDCLNT_SHAREMODE_EXCLUSIVE
                                        : AUDCLNT_SHAREMODE_SHARED;

    WAVEFORMATEXTENSIBLE wf{};
    SampleFormat         wire = desired.format;

    if (share == AUDCLNT_SHAREMODE_EXCLUSIVE) {
        // Try the source's own depth first, then the depths a consumer endpoint
        // is most likely to accept. The RATE is never negotiated -- that is #32:
        // if the endpoint will not take this rate, the answer is to say so, not
        // to resample and call it bit-perfect.
        const SampleFormat candidates[] = {desired.format, SampleFormat::kInt24,
                                           SampleFormat::kInt32, SampleFormat::kInt16,
                                           SampleFormat::kFloat32};
        bool accepted = false;
        HRESULT last  = AUDCLNT_E_UNSUPPORTED_FORMAT;

        for (const SampleFormat c : candidates) {
            fill_format(wf, desired.sample_rate, desired.channels, c);
            last = impl_->client->IsFormatSupported(share,
                                                    reinterpret_cast<WAVEFORMATEX*>(&wf), nullptr);
            if (last == S_OK) {
                wire     = c;
                accepted = true;
                break;
            }
            if (last == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED) {
                impl_->release();
                return SinkError::kExclusiveModeNotPermitted;
            }
        }

        if (!accepted) {
            impl_->release();
            // Every depth was refused at this rate. Per #32 that is reported,
            // not worked around: the caller can retry at another rate or in
            // shared mode, and either way it KNOWS.
            return SinkError::kRateUnavailable;
        }
    } else {
        // Shared mode goes through the mixer, which has one format and will not
        // negotiate. Use it verbatim.
        WAVEFORMATEX* mix = nullptr;
        hr = impl_->client->GetMixFormat(&mix);
        if (FAILED(hr) || mix == nullptr) {
            impl_->release();
            return SinkError::kBackendFailure;
        }
        std::memcpy(&wf, mix, std::min<std::size_t>(sizeof(wf), sizeof(WAVEFORMATEX) + mix->cbSize));
        wire = SampleFormat::kFloat32;
        if (mix->wBitsPerSample == 16) {
            wire = SampleFormat::kInt16;
        }
        CoTaskMemFree(mix);
    }

    REFERENCE_TIME default_period = 0;
    REFERENCE_TIME min_period     = 0;
    impl_->client->GetDevicePeriod(&default_period, &min_period);
    const REFERENCE_TIME want_period =
        (share == AUDCLNT_SHAREMODE_EXCLUSIVE) ? min_period : default_period;

    hr = impl_->client->Initialize(share, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, want_period,
                                   (share == AUDCLNT_SHAREMODE_EXCLUSIVE) ? want_period : 0,
                                   reinterpret_cast<WAVEFORMATEX*>(&wf), nullptr);

    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        // Documented WASAPI dance: the device tells us the aligned size, and we
        // must throw the client away and rebuild with it. Not an error path --
        // it is the expected path on plenty of hardware.
        UINT32 aligned = 0;
        if (SUCCEEDED(impl_->client->GetBufferSize(&aligned)) && aligned > 0) {
            const REFERENCE_TIME realigned = static_cast<REFERENCE_TIME>(
                10000.0 * 1000 * aligned / wf.Format.nSamplesPerSec + 0.5);

            impl_->client->Release();
            impl_->client = nullptr;
            hr = impl_->device->Activate(kIAudioClient, CLSCTX_ALL, nullptr,
                                         reinterpret_cast<void**>(&impl_->client));
            if (SUCCEEDED(hr)) {
                hr = impl_->client->Initialize(share, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                               realigned, realigned,
                                               reinterpret_cast<WAVEFORMATEX*>(&wf), nullptr);
            }
        }
    }

    if (FAILED(hr)) {
        const SinkError err = classify(hr);
        impl_->release();
        return err;
    }

    UINT32 buffer_frames = 0;
    if (FAILED(impl_->client->GetBufferSize(&buffer_frames)) || buffer_frames == 0) {
        impl_->release();
        return SinkError::kBackendFailure;
    }

    impl_->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (impl_->event == nullptr || FAILED(impl_->client->SetEventHandle(impl_->event))) {
        impl_->release();
        return SinkError::kBackendFailure;
    }

    if (FAILED(impl_->client->GetService(kIAudioRenderClient,
                                         reinterpret_cast<void**>(&impl_->render)))) {
        impl_->release();
        return SinkError::kBackendFailure;
    }
    if (FAILED(impl_->client->GetService(kIAudioClock,
                                         reinterpret_cast<void**>(&impl_->clock)))) {
        impl_->release();
        return SinkError::kBackendFailure;
    }
    impl_->clock->GetFrequency(&impl_->clock_freq);

    impl_->period = buffer_frames;
    impl_->wire   = wire;
    impl_->cb     = cb;
    impl_->user   = user;

    impl_->fmt.sample_rate = wf.Format.nSamplesPerSec;
    impl_->fmt.channels    = static_cast<std::uint16_t>(wf.Format.nChannels);
    impl_->fmt.format      = wire;

    // The honest answer to "is this bit-perfect", computed rather than assumed.
    // Exclusive mode is necessary but not sufficient: a 32-bit integer wire
    // format cannot round-trip through float (see sample_convert.hpp and #36),
    // so it is excluded here even though the mixer is out of the way.
    impl_->bit_perfect = (share == AUDCLNT_SHAREMODE_EXCLUSIVE) &&
                         (impl_->fmt.sample_rate == desired.sample_rate) &&
                         (wire == SampleFormat::kInt16 || wire == SampleFormat::kInt24 ||
                          wire == SampleFormat::kFloat32);

    impl_->scratch.assign(static_cast<std::size_t>(impl_->period) * impl_->fmt.channels, 0.0f);
    impl_->late.store(0, std::memory_order_relaxed);
    impl_->open_ = true;
    return SinkError::kOk;
}

void WasapiSink::close()
{
    if (!impl_->open_) {
        return;
    }
    stop();
    if (impl_->client != nullptr) {
        impl_->client->Reset();
    }
    impl_->release();
    impl_->scratch.clear();
    impl_->open_       = false;
    impl_->bit_perfect = false;
    impl_->period      = 0;
    impl_->cb          = nullptr;
    impl_->user        = nullptr;
    impl_->fmt         = SinkFormat{};

    if (impl_->com_initialised) {
        CoUninitialize();
        impl_->com_initialised = false;
    }
}

SinkError WasapiSink::start()
{
    if (!impl_->open_) {
        return SinkError::kNotOpen;
    }
    if (impl_->running.load(std::memory_order_acquire)) {
        return SinkError::kOk;
    }

    // Prime the first period BEFORE starting the clock. An exclusive-mode stream
    // that starts with an empty buffer glitches immediately, every time.
    BYTE* buffer = nullptr;
    if (SUCCEEDED(impl_->render->GetBuffer(impl_->period, &buffer))) {
        if (impl_->cb != nullptr) {
            impl_->cb(impl_->scratch.data(), impl_->period, impl_->fmt.channels, impl_->user);
        }
        impl_->convert(impl_->scratch.data(), buffer, impl_->period);
        impl_->render->ReleaseBuffer(impl_->period, 0);
    }

    impl_->running.store(true, std::memory_order_release);
    impl_->worker = std::thread([this] { impl_->run(); });

    if (FAILED(impl_->client->Start())) {
        impl_->running.store(false, std::memory_order_release);
        if (impl_->worker.joinable()) {
            SetEvent(impl_->event);
            impl_->worker.join();
        }
        return SinkError::kBackendFailure;
    }
    return SinkError::kOk;
}

SinkError WasapiSink::stop()
{
    if (!impl_->open_) {
        return SinkError::kNotOpen;
    }
    if (!impl_->running.load(std::memory_order_acquire)) {
        return SinkError::kOk;
    }

    impl_->running.store(false, std::memory_order_release);
    // Wake the loop so it observes the flag now rather than after another
    // period. Joining is what makes the interface's promise true: when stop()
    // returns, the callback is definitively not running.
    SetEvent(impl_->event);
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
    impl_->client->Stop();
    return SinkError::kOk;
}

bool WasapiSink::is_open() const { return impl_->open_; }
bool WasapiSink::is_running() const { return impl_->running.load(std::memory_order_acquire); }

SinkFormat    WasapiSink::format()        const { return impl_->fmt; }
std::uint32_t WasapiSink::period_frames() const { return impl_->period; }

SinkClock WasapiSink::clock() const
{
    SinkClock c{};
    if (!impl_->open_ || impl_->clock == nullptr || impl_->clock_freq == 0) {
        return c;
    }

    UINT64 position = 0;
    UINT64 qpc      = 0;
    if (FAILED(impl_->clock->GetPosition(&position, &qpc))) {
        return c;
    }

    // THE REASON THIS BACKEND EXISTS. Not derived, not inferred: the device
    // reports the position it has actually played and the QPC instant at which
    // that was true. Exactly the correlated pair audio_sink.hpp specifies and
    // the thing #53 needs to place the analysis tap.
    c.frames_played = (position * impl_->fmt.sample_rate) / impl_->clock_freq;

    // qpc is in 100-nanosecond units, per IAudioClock::GetPosition.
    c.timestamp_seconds = static_cast<double>(qpc) / 10'000'000.0;
    c.valid             = true;
    return c;
}

const char* WasapiSink::backend_name() const
{
    return (impl_->mode == WasapiMode::kExclusive) ? "wasapi-exclusive" : "wasapi-shared";
}

std::uint64_t WasapiSink::late_periods() const
{
    return impl_->late.load(std::memory_order_relaxed);
}

#else  // !_WIN32

// The non-Windows build. Every call fails cleanly rather than the type
// vanishing, so callers need no preprocessor conditionals and the Linux job
// still compiles and links this file.

struct WasapiSink::Impl {
    WasapiMode mode = WasapiMode::kExclusive;
};

WasapiSink::WasapiSink() : impl_(std::make_unique<Impl>()) {}
WasapiSink::~WasapiSink() = default;

bool WasapiSink::available() { return false; }

void       WasapiSink::set_mode(WasapiMode m) { impl_->mode = m; }
WasapiMode WasapiSink::mode() const           { return impl_->mode; }
bool        WasapiSink::is_bit_perfect() const { return false; }
const char* WasapiSink::bit_perfect_note() const { return "WASAPI is Windows-only"; }

SinkError WasapiSink::open(const SinkFormat& desired, RenderCallback cb, void*)
{
    // The SAME argument validation as the Windows path, and it is not
    // decoration. The Linux job caught this: the stub returned kDeviceNotFound
    // for a null callback while Windows returned kFormatUnsupported, so a
    // caller checking "did I pass something malformed" got different answers
    // per platform from one interface.
    //
    // Argument validation is a property of the CALL, not of the hardware. It
    // must not depend on whether a device could ever have existed.
    if (cb == nullptr || desired.channels == 0 || desired.sample_rate == 0) {
        return SinkError::kFormatUnsupported;
    }
    return SinkError::kDeviceNotFound;
}
void      WasapiSink::close() {}
SinkError WasapiSink::start() { return SinkError::kNotOpen; }
SinkError WasapiSink::stop()  { return SinkError::kNotOpen; }

bool WasapiSink::is_open()    const { return false; }
bool WasapiSink::is_running() const { return false; }

SinkFormat    WasapiSink::format()        const { return SinkFormat{}; }
std::uint32_t WasapiSink::period_frames() const { return 0; }
SinkClock     WasapiSink::clock()         const { return SinkClock{}; }

const char*   WasapiSink::backend_name() const { return "wasapi-unavailable"; }
std::uint64_t WasapiSink::late_periods() const { return 0; }

#endif  // _WIN32

}  // namespace holocron
