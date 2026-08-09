// SPDX-License-Identifier: GPL-3.0-or-later
//
// The decoder and the tap resampler.
//
// Fixtures are WRITTEN, not committed. .gitignore blocks media formats for good
// reasons (a repo full of audio is a licensing and size problem), and a test
// that generates its own input is also a test whose input you can read. Each
// case writes a WAV with known content, decodes it back, and asserts on what
// came out.

#include <holocron/audio_frame.hpp>
#include <holocron/decoder.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace holocron;
using Catch::Approx;

namespace {

constexpr double kPi = 3.14159265358979323846;

// A temp file that removes itself, so a failing assertion does not leave
// litter in the system temp directory.
struct ScopedFile {
    explicit ScopedFile(const char* stem)
    {
        path = (std::filesystem::temp_directory_path() /
                (std::string("holocron_test_") + stem + ".wav"))
                   .string();
    }
    ~ScopedFile()
    {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    std::string path;
};

void put32(std::ofstream& o, std::uint32_t v) { o.write(reinterpret_cast<const char*>(&v), 4); }
void put16(std::ofstream& o, std::uint16_t v) { o.write(reinterpret_cast<const char*>(&v), 2); }

// Minimal 16-bit PCM WAV. Hand-rolled rather than muxed through FFmpeg, so a
// decoder bug cannot be masked by the same library writing the fixture.
void write_wav(const std::string& path, std::uint32_t rate, std::uint16_t channels,
               const std::vector<float>& interleaved)
{
    std::ofstream o(path, std::ios::binary);
    REQUIRE(o.good());

    const std::uint32_t samples   = std::uint32_t(interleaved.size());
    const std::uint32_t data_size = samples * 2;
    const std::uint16_t block     = std::uint16_t(channels * 2);

    o.write("RIFF", 4);
    put32(o, 36 + data_size);
    o.write("WAVE", 4);

    o.write("fmt ", 4);
    put32(o, 16);
    put16(o, 1);  // PCM
    put16(o, channels);
    put32(o, rate);
    put32(o, rate * block);
    put16(o, block);
    put16(o, 16);

    o.write("data", 4);
    put32(o, data_size);

    for (float v : interleaved) {
        const float   clamped = (v > 1.0f) ? 1.0f : (v < -1.0f ? -1.0f : v);
        const auto    s       = std::int16_t(std::lround(double(clamped) * 32767.0));
        o.write(reinterpret_cast<const char*>(&s), 2);
    }
    o.close();
}

std::vector<float> sine(double hz, std::uint32_t rate, std::uint16_t channels, double seconds,
                        float amplitude = 0.8f)
{
    const std::size_t  frames = std::size_t(seconds * double(rate));
    std::vector<float> out(frames * channels);
    for (std::size_t i = 0; i < frames; ++i) {
        const auto v = float(double(amplitude) * std::sin(2.0 * kPi * hz * double(i) / double(rate)));
        for (std::uint16_t c = 0; c < channels; ++c) {
            out[i * channels + c] = v;
        }
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------

TEST_CASE("a fresh decoder is closed", "[decoder]")
{
    Decoder d;
    CHECK_FALSE(d.is_open());
    CHECK(d.info().sample_rate == 0u);
}

TEST_CASE("opening a missing file reports it distinguishably", "[decoder][errors]")
{
    Decoder d;
    CHECK(d.open("this_file_does_not_exist_12345.flac") == DecoderError::kFileNotFound);
    CHECK_FALSE(d.is_open());
}

TEST_CASE("opening a non-audio file does not report file-not-found", "[decoder][errors]")
{
    // The distinction matters: "no such file" and "that is not audio" need
    // different messages to a user, which is why DecoderError is an enum.
    const auto path = (std::filesystem::temp_directory_path() / "holocron_not_audio.txt").string();
    {
        std::ofstream o(path, std::ios::binary);
        o << "this is definitely not an audio file, it is just some text\n";
    }

    Decoder    d;
    const auto err = d.open(path.c_str());
    INFO("got: " << to_string(err));
    CHECK(err != DecoderError::kOk);
    CHECK(err != DecoderError::kFileNotFound);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("every DecoderError has a distinct description", "[decoder][errors]")
{
    const DecoderError all[] = {
        DecoderError::kOk,          DecoderError::kFileNotFound, DecoderError::kNotAudio,
        DecoderError::kUnsupportedCodec, DecoderError::kCorruptStream,
        DecoderError::kAlreadyOpen, DecoderError::kNotOpen,      DecoderError::kBackendFailure,
    };
    std::vector<std::string> seen;
    for (DecoderError e : all) {
        const std::string s = to_string(e);
        CHECK_FALSE(s.empty());
        CHECK(s != "unknown");
        for (const auto& prior : seen) {
            CHECK(s != prior);
        }
        seen.push_back(s);
    }
}

TEST_CASE("a WAV round-trips through the decoder", "[decoder]")
{
    ScopedFile f("roundtrip");
    write_wav(f.path, 44100, 2, sine(440.0, 44100, 2, 0.5));

    Decoder d;
    REQUIRE(d.open(f.path.c_str()) == DecoderError::kOk);
    REQUIRE(d.is_open());

    const SourceInfo info = d.info();
    CHECK(info.sample_rate == 44100u);
    CHECK(info.channels == 2u);
    CHECK(info.is_lossless);
    INFO("codec: " << info.codec_name);

    std::vector<float> out(4096 * 2);
    std::size_t        total = 0;
    float              peak  = 0.0f;
    while (true) {
        const std::size_t got = d.read(out.data(), 4096);
        if (got == 0) {
            break;
        }
        for (std::size_t i = 0; i < got * 2; ++i) {
            peak = std::max(peak, std::abs(out[i]));
        }
        total += got;
    }

    // 0.5 s at 44.1 kHz.
    CHECK(total == Approx(22050.0).margin(64));
    // Written at 0.8 amplitude; 16-bit quantisation costs a hair.
    CHECK(peak == Approx(0.8f).margin(0.01));
}

TEST_CASE("double open is rejected rather than leaking the first", "[decoder][errors]")
{
    ScopedFile f("double");
    write_wav(f.path, 48000, 2, sine(440.0, 48000, 2, 0.1));

    Decoder d;
    REQUIRE(d.open(f.path.c_str()) == DecoderError::kOk);
    CHECK(d.open(f.path.c_str()) == DecoderError::kAlreadyOpen);
    CHECK(d.is_open());
}

TEST_CASE("close then reopen works", "[decoder]")
{
    ScopedFile f("reopen");
    write_wav(f.path, 48000, 2, sine(440.0, 48000, 2, 0.1));

    Decoder d;
    REQUIRE(d.open(f.path.c_str()) == DecoderError::kOk);
    d.close();
    CHECK_FALSE(d.is_open());
    CHECK(d.open(f.path.c_str()) == DecoderError::kOk);
}

TEST_CASE("mono files decode as mono", "[decoder]")
{
    ScopedFile f("mono");
    write_wav(f.path, 48000, 1, sine(440.0, 48000, 1, 0.2));

    Decoder d;
    REQUIRE(d.open(f.path.c_str()) == DecoderError::kOk);
    CHECK(d.info().channels == 1u);

    std::vector<float> out(4096);
    CHECK(d.read(out.data(), 4096) > 0u);
}

// ---------------------------------------------------------------------------
// Resampler
// ---------------------------------------------------------------------------

TEST_CASE("the resampler converts to the fixed analysis rate", "[resampler]")
{
    // The property D-004 exists for: whatever the file is, the tap is 48 kHz.
    Resampler r;
    REQUIRE(r.configure(44100, 2) == DecoderError::kOk);
    REQUIRE(r.is_configured());

    const std::size_t  in_frames = 44100;  // one second
    std::vector<float> in(in_frames * 2, 0.0f);
    for (std::size_t i = 0; i < in_frames; ++i) {
        const auto v = float(0.5 * std::sin(2.0 * kPi * 440.0 * double(i) / 44100.0));
        in[i * 2 + 0] = v;
        in[i * 2 + 1] = v;
    }

    std::vector<float> out(r.max_output_frames(in_frames) * 2 + 64);
    std::size_t        produced = r.process(in.data(), in_frames, out.data(), out.size() / 2);
    produced += r.flush(out.data() + produced * 2, out.size() / 2 - produced);

    // One second in, one second out at the analysis rate.
    INFO("produced " << produced << " frames");
    CHECK(double(produced) == Approx(double(kAnalysisRate)).margin(64.0));
}

TEST_CASE("48 kHz passes through at the same length", "[resampler]")
{
    Resampler r;
    REQUIRE(r.configure(kAnalysisRate, 2) == DecoderError::kOk);

    const std::size_t  n = 4800;
    std::vector<float> in(n * 2, 0.25f);
    std::vector<float> out(r.max_output_frames(n) * 2 + 64);

    std::size_t produced = r.process(in.data(), n, out.data(), out.size() / 2);
    produced += r.flush(out.data() + produced * 2, out.size() / 2 - produced);

    CHECK(double(produced) == Approx(double(n)).margin(8.0));
}

TEST_CASE("mono is upmixed to the stereo tap", "[resampler]")
{
    // The analysis stage assumes stereo unconditionally, so the resampler owns
    // making that true rather than every caller checking.
    Resampler r;
    REQUIRE(r.configure(48000, 1) == DecoderError::kOk);

    const std::size_t  n = 1000;
    std::vector<float> in(n, 0.5f);
    std::vector<float> out(r.max_output_frames(n) * 2 + 64);

    const std::size_t produced = r.process(in.data(), n, out.data(), out.size() / 2);
    REQUIRE(produced > 0u);

    // Stereo interleaved: both channels present and equal for a mono source.
    CHECK(out[0] == Approx(out[1]).margin(1e-5));
}

TEST_CASE("an unconfigured resampler produces nothing", "[resampler]")
{
    Resampler          r;
    std::vector<float> out(128);
    CHECK_FALSE(r.is_configured());
    CHECK(r.process(nullptr, 0, out.data(), 64) == 0u);
    CHECK(r.max_output_frames(1000) == 0u);
}

TEST_CASE("reconfiguring is allowed", "[resampler]")
{
    Resampler r;
    REQUIRE(r.configure(44100, 2) == DecoderError::kOk);
    REQUIRE(r.configure(96000, 2) == DecoderError::kOk);
    CHECK(r.is_configured());
}

TEST_CASE("configure rejects nonsense", "[resampler][errors]")
{
    Resampler r;
    CHECK(r.configure(0, 2) == DecoderError::kBackendFailure);
    CHECK(r.configure(48000, 0) == DecoderError::kBackendFailure);
}

// ---------------------------------------------------------------------------

TEST_CASE("decoder and resampler compose into the analysis tap", "[decoder][resampler]")
{
    // The whole point: a 44.1 kHz file becomes a 48 kHz stereo stream the
    // analysis stage can consume without knowing what the source was.
    ScopedFile f("tap");
    write_wav(f.path, 44100, 2, sine(1000.0, 44100, 2, 1.0));

    Decoder d;
    REQUIRE(d.open(f.path.c_str()) == DecoderError::kOk);

    const SourceInfo info = d.info();
    Resampler        r;
    REQUIRE(r.configure(info.sample_rate, info.channels) == DecoderError::kOk);

    std::vector<float> native(4096 * info.channels);
    std::vector<float> tapped(r.max_output_frames(4096) * 2 + 64);
    std::size_t        tap_frames = 0;

    while (true) {
        const std::size_t got = d.read(native.data(), 4096);
        if (got == 0) {
            break;
        }
        tap_frames += r.process(native.data(), got, tapped.data(), tapped.size() / 2);
    }
    tap_frames += r.flush(tapped.data(), tapped.size() / 2);

    // One second of 44.1 kHz source becomes one second of 48 kHz tap.
    INFO("tap frames: " << tap_frames);
    CHECK(double(tap_frames) == Approx(double(kAnalysisRate)).margin(128.0));
}

// ---------------------------------------------------------------------------
// Seeking
//
// A RAMP RATHER THAN A SINE, so a sample's VALUE says where in the file it came
// from. A sine is periodic, so landing a whole second early reads identically to
// landing on target and the test would pass while seeking was broken.
//
// PCM WAV has no keyframes, so these seeks are sample-accurate. A compressed
// format would land at or before the request instead -- which is what
// AVSEEK_FLAG_BACKWARD guarantees and why the session winds the remainder
// forward.
// ---------------------------------------------------------------------------

namespace {

constexpr std::uint32_t kRampRate    = 8000;
constexpr double        kRampSeconds = 4.0;

// value at frame i is i / total, so 0.0 at the start and ~1.0 at the end.
std::vector<float> ramp()
{
    const std::size_t  frames = std::size_t(kRampSeconds * double(kRampRate));
    std::vector<float> out(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        out[i] = float(double(i) / double(frames));
    }
    return out;
}

// The first sample the decoder yields, which for a ramp names the position.
float first_sample(Decoder& d)
{
    float value = -1.0f;
    REQUIRE(d.read(&value, 1) == 1);
    return value;
}

}  // namespace

TEST_CASE("a local file reports itself seekable", "[decoder][seek]")
{
    ScopedFile f("seekable");
    write_wav(f.path, kRampRate, 1, ramp());

    Decoder d;
    REQUIRE(d.open(f.path.c_str()) == DecoderError::kOk);

    // False is a real answer for a pipe or a stream that refuses range requests,
    // which is why the session asks before choosing how to reach an offset.
    CHECK(d.can_seek());
}

TEST_CASE("a closed decoder can neither seek nor claim to", "[decoder][seek][errors]")
{
    Decoder d;
    CHECK_FALSE(d.can_seek());
    CHECK(d.seek(1.0) == DecoderError::kNotOpen);
}

TEST_CASE("seeking lands where it was asked to", "[decoder][seek]")
{
    ScopedFile f("seek_position");
    write_wav(f.path, kRampRate, 1, ramp());

    Decoder d;
    REQUIRE(d.open(f.path.c_str()) == DecoderError::kOk);

    // Halfway through a 4-second ramp is 0.5.
    REQUIRE(d.seek(2.0) == DecoderError::kOk);
    CHECK(first_sample(d) == Approx(0.5).margin(0.01));

    // Three quarters.
    REQUIRE(d.seek(3.0) == DecoderError::kOk);
    CHECK(first_sample(d) == Approx(0.75).margin(0.01));
}

TEST_CASE("seeking backwards works, not just forwards", "[decoder][seek]")
{
    // The direction that catches a decoder whose buffers were not flushed: the
    // frames it still holds are from AFTER the new position, so playback resumes
    // where it used to be and sounds like the seek did nothing.
    ScopedFile f("seek_backwards");
    write_wav(f.path, kRampRate, 1, ramp());

    Decoder d;
    REQUIRE(d.open(f.path.c_str()) == DecoderError::kOk);

    REQUIRE(d.seek(3.0) == DecoderError::kOk);
    CHECK(first_sample(d) == Approx(0.75).margin(0.01));

    REQUIRE(d.seek(1.0) == DecoderError::kOk);
    CHECK(first_sample(d) == Approx(0.25).margin(0.01));
}

TEST_CASE("seeking to zero returns to the top of the track", "[decoder][seek]")
{
    ScopedFile f("seek_zero");
    write_wav(f.path, kRampRate, 1, ramp());

    Decoder d;
    REQUIRE(d.open(f.path.c_str()) == DecoderError::kOk);

    std::vector<float> scratch(kRampRate);
    REQUIRE(d.read(scratch.data(), scratch.size()) > 0);

    REQUIRE(d.seek(0.0) == DecoderError::kOk);
    CHECK(first_sample(d) == Approx(0.0).margin(0.01));
}

TEST_CASE("a negative seek is clamped rather than refused", "[decoder][seek]")
{
    // A controller can send one, and the start of the track is the only sensible
    // reading of it.
    ScopedFile f("seek_negative");
    write_wav(f.path, kRampRate, 1, ramp());

    Decoder d;
    REQUIRE(d.open(f.path.c_str()) == DecoderError::kOk);

    REQUIRE(d.seek(-5.0) == DecoderError::kOk);
    CHECK(first_sample(d) == Approx(0.0).margin(0.01));
}

TEST_CASE("seeking clears end of stream so the decoder is reusable",
          "[decoder][seek]")
{
    // Reading to the end sets eof and drained. Seeking back has to clear both or
    // the decoder reports the track over while sitting at its start -- which in
    // the player is a seek that lands correctly and then immediately advances to
    // the next track.
    ScopedFile f("seek_after_eof");
    write_wav(f.path, kRampRate, 1, ramp());

    Decoder d;
    REQUIRE(d.open(f.path.c_str()) == DecoderError::kOk);

    std::vector<float> scratch(4096);
    while (d.read(scratch.data(), scratch.size()) > 0) {
        // drain
    }
    REQUIRE(d.at_end());

    REQUIRE(d.seek(1.0) == DecoderError::kOk);
    CHECK_FALSE(d.at_end());
    CHECK(first_sample(d) == Approx(0.25).margin(0.01));
}

TEST_CASE("seeking past the end leaves nothing to read", "[decoder][seek]")
{
    // Not an error: the player clamps a scrub to just inside the track, and this
    // is the behaviour that makes that clamp the only thing needed.
    ScopedFile f("seek_past_end");
    write_wav(f.path, kRampRate, 1, ramp());

    Decoder d;
    REQUIRE(d.open(f.path.c_str()) == DecoderError::kOk);

    d.seek(kRampSeconds + 30.0);

    std::vector<float> scratch(1024);
    CHECK(d.read(scratch.data(), scratch.size()) == 0);
}

// ---------------------------------------------------------------------------
// Tag encoding, issue 133
//
// The failure this prevents produces no error, no crash and no wrong-looking
// data structure: just wrong characters on a screen. Tag bytes are arbitrary --
// ID3v2.3 defaults to Latin-1 and plenty of rippers write whatever the system
// codepage was -- while everything downstream assumes UTF-8. A tag that cannot be
// vouched for is dropped so the caller falls back to the filename.
//
// Exactly the class of bug that needs a test rather than a careful reading, and
// the same class CLAUDE.md already warns about for PowerShell edits.
// ---------------------------------------------------------------------------

TEST_CASE("plain ASCII is valid UTF-8", "[decoder][tags]")
{
    CHECK(is_valid_utf8(""));
    CHECK(is_valid_utf8("Stinkfist"));
    CHECK(is_valid_utf8("01. Skrillex - First Of The Year (Equinox)"));
}

TEST_CASE("real UTF-8 from the rack is accepted", "[decoder][tags]")
{
    // Both of these are real values from the owner's library and both appear in
    // this project's logs. AEnima is two bytes, the curly apostrophe is three.
    CHECK(is_valid_utf8("\xC3\x86nima"));                      // Ænima
    CHECK(is_valid_utf8("Rock n\xE2\x80\x99 Roll"));           // Rock n’ Roll
    CHECK(is_valid_utf8("Forty Six \xE2\x80\x93 2"));          // en dash
    CHECK(is_valid_utf8("\xF0\x9F\x8E\xB5"));                  // a 4-byte emoji
}

TEST_CASE("Latin-1 bytes are refused", "[decoder][tags]")
{
    // THE CASE THAT MATTERS. 0xC6 is 'AE' in Latin-1 and a 2-byte UTF-8 lead
    // byte, so "\xC6nima" looks like the start of a sequence and is followed by
    // 'n' -- not a continuation byte. Passed through, this is mojibake on the
    // now-playing card.
    CHECK_FALSE(is_valid_utf8("\xC6nima"));

    // A lone high byte, which is what a Windows-1252 curly apostrophe is.
    CHECK_FALSE(is_valid_utf8("Rock n\x92 Roll"));
}

TEST_CASE("malformed UTF-8 is refused", "[decoder][tags]")
{
    CHECK_FALSE(is_valid_utf8("\x80"));              // a bare continuation byte
    CHECK_FALSE(is_valid_utf8("\xE2\x80"));          // truncated 3-byte sequence
    CHECK_FALSE(is_valid_utf8("\xF0\x9F\x8E"));      // truncated 4-byte sequence
    CHECK_FALSE(is_valid_utf8("\xFF\xFE"));          // invalid lead bytes
    CHECK_FALSE(is_valid_utf8("\xC0\x80"));          // overlong encoding of NUL
}

TEST_CASE("a null tag is refused rather than dereferenced", "[decoder][tags]")
{
    // av_dict_get can hand back an entry whose value is null.
    CHECK_FALSE(is_valid_utf8(nullptr));
}

TEST_CASE("a file with no tags leaves the fields empty rather than inventing them",
          "[decoder][tags]")
{
    // The hand-rolled WAV writer emits no metadata chunk at all, which is exactly
    // the case the filename fallback exists for. Empty is the correct answer here;
    // anything else would be the decoder making something up.
    ScopedFile f("no_tags");
    write_wav(f.path, 48000, 2, sine(440.0, 48000, 2, 0.2));

    Decoder d;
    REQUIRE(d.open(f.path.c_str()) == DecoderError::kOk);

    const SourceInfo info = d.info();
    CHECK(info.title.empty());
    CHECK(info.artist.empty());
    CHECK(info.album.empty());
    CHECK(info.genre.empty());
    CHECK(info.year.empty());

    // And the fields that do not come from tags are still populated, so an empty
    // title is not a symptom of the probe having failed.
    CHECK(info.sample_rate == 48000u);
    CHECK(info.channels == 2);
}
