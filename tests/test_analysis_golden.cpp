// SPDX-License-Identifier: GPL-3.0-or-later
//
// The golden-file diff for the headless analysis. M1's seventh exit criterion.
//
// WHAT THIS CATCHES THAT NOTHING ELSE DOES. Every other analysis test asserts a
// PROPERTY -- that rms rises with level, that beat_phase wraps, that a silent
// buffer produces silence. Properties survive a change that is wrong: shift
// every band edge by one bin, or change a window function, and every property
// still holds while every crystal in the vault quietly looks different. What
// says so is a frame-by-frame record of what the analysis used to produce.
//
// -- how to regenerate it, because you will need to ---------------------------
//
// When the analysis is deliberately changed, this test SHOULD fail. Look at the
// diff first -- that is the point of it -- then:
//
//     HOLOCRON_WRITE_GOLDEN=1 ctest --preset windows -R "golden"
//
// which rewrites tests/fixtures/analysis-golden.csv in the source tree and
// prints the path. Commit the regenerated file IN THE SAME COMMIT as the
// analysis change, so `git show` on that commit is the record of what moved.
// The failure message says all of this too.
//
// -- the fixture, and why it is generated rather than committed ---------------
//
// .gitignore blocks media formats and tests/test_decoder.cpp already established
// the alternative: a test that generates its own input is also a test whose
// input you can read. The signal is eight seconds built from four things chosen
// for what they exercise:
//
//   a quarter second of true silence   the start of a track, which is where both
//                                      bugs in #44 lived and where no unit test
//                                      was looking. loudness_short reports its
//                                      -70 floor, the auto-gain has no history,
//                                      and every array digest is the all-zero
//                                      case
//   three sustained tones              80 Hz, 1 kHz and 6 kHz, so bass, mid and
//                                      treble all carry content and the spectral
//                                      descriptors have somewhere to sit
//   a click every half second          exactly 120 BPM, from 0.5 s. Sharp enough
//                                      to be an onset, regular enough that the
//                                      tempo autocorrelation has an unambiguous
//                                      peak rather than a near-tie two adjacent
//                                      lags could flip between
//   a drop to 30% at four seconds      the auto-gain, the band normalisation and
//                                      the loudness window all have to respond,
//                                      and half the file is on each side of it
//
// The two channels differ -- the right one's mid tone is phase-shifted and its
// treble is quieter -- because `stereo_correlation` and `stereo_width` are real
// fields and a mono fixture pins them at 1 and 0 for the whole run, which is
// indistinguishable from them being broken.
//
// -- and why the comparison has a tolerance ------------------------------------
//
// Byte-for-byte would be better and does not work. Floating point is not
// bit-identical across compilers: MSVC and gcc vectorise differently, contract
// differently into FMAs, and evaluate the same expression to different last
// bits. This project keeps Linux CI precisely BECAUSE the two disagree, so a
// golden that demanded they agree exactly would fail on every push for a reason
// that is not a regression.
//
// So integer columns are compared exactly and float columns within
// `kRelativeTolerance` -- four orders of magnitude tighter than any analysis
// change anyone would make on purpose, and wide enough that last-bit divergence
// cannot reach it. The `analysis golden comparison can fail` case below is what
// stops that from being a claim: it runs the same fixture through a config with
// one envelope constant moved by 4% and requires the comparison to reject it.

#include <holocron/analysis.hpp>
#include <holocron/audio_frame.hpp>
#include <holocron/decoder.hpp>
#include <holocron/frame_csv.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace holocron;

namespace {

constexpr double kPi = 3.14159265358979323846;

const char* golden_path() { return HOLOCRON_SOURCE_DIR "/tests/fixtures/analysis-golden.csv"; }

// ---------------------------------------------------------------------------
// The fixture signal.
// ---------------------------------------------------------------------------

constexpr double kFixtureSeconds = 8.0;
constexpr double kSilentUntil    = 0.25;   // s
constexpr double kFirstClick     = 0.5;    // s
constexpr double kClickPeriod    = 0.5;    // s -- 120 BPM
constexpr double kQuietFrom      = 4.0;    // s
constexpr double kQuietGain      = 0.3;

// One click: a 2.5 kHz burst with a 4 ms decay. Broadband enough at its leading
// edge to move the spectral flux, short enough not to smear into the next one.
double click_at(double u)
{
    if (u < 0.0 || u >= 0.05) {
        return 0.0;
    }
    return 0.45 * std::exp(-u / 0.004) * std::sin(2.0 * kPi * 2500.0 * u);
}

// QUANTISED TO 16 BITS EVEN THOUGH NOTHING HERE WRITES A FILE.
//
// The second test below runs the same signal through a WAV, the decoder and the
// resampler, and compares against the SAME golden. That only means anything if
// the samples are identical on both paths, so the float path is quantised the
// way the WAV path will be -- lround(v * 32767) on the way out, /32768 on the
// way back, which is what tests/test_decoder.cpp writes and what FFmpeg reads.
float quantise(double v)
{
    const double clamped = (v > 1.0) ? 1.0 : (v < -1.0 ? -1.0 : v);
    return float(double(std::lround(clamped * 32767.0)) / 32768.0);
}

std::vector<float> fixture_signal()
{
    const std::size_t  frames = std::size_t(kFixtureSeconds * double(kAnalysisRate));
    std::vector<float> out(frames * 2);

    for (std::size_t i = 0; i < frames; ++i) {
        const double t = double(i) / double(kAnalysisRate);

        double left  = 0.0;
        double right = 0.0;

        if (t >= kSilentUntil) {
            const double gain = (t < kQuietFrom) ? 1.0 : kQuietGain;

            const double bass   = 0.30 * std::sin(2.0 * kPi * 80.0 * t);
            const double mid_l  = 0.20 * std::sin(2.0 * kPi * 1000.0 * t);
            const double mid_r  = 0.20 * std::sin(2.0 * kPi * 1000.0 * t + kPi / 3.0);
            const double treb   = 0.10 * std::sin(2.0 * kPi * 6000.0 * t);

            left  = gain * (bass + mid_l + treb);
            right = gain * (bass + mid_r + 0.4 * treb);
        }

        // Which click, if any, is currently sounding. Only ever one: the period
        // is 500 ms and a click is 50 ms long.
        if (t >= kFirstClick) {
            const double k = std::floor((t - kFirstClick) / kClickPeriod);
            const double c = click_at(t - (kFirstClick + k * kClickPeriod));
            left += c;
            right += c;
        }

        out[i * 2]     = quantise(left);
        out[i * 2 + 1] = quantise(right);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Running the stage and collecting rows.
// ---------------------------------------------------------------------------

struct Collector {
    std::vector<std::string> rows;
};

void collect(const AudioFrame& f, void* user)
{
    auto&             c = *static_cast<Collector*>(user);
    char              line[kFrameCsvRowMax];
    const std::size_t n = format_frame_csv(f, line, sizeof(line));
    REQUIRE(n > 0);
    c.rows.emplace_back(line, n);
}

// Push in chunks rather than in one call, because that is what the harness and
// the player both do, and because AnalysisStage promises the two are identical.
std::vector<std::string> run_analysis(const std::vector<float>& interleaved,
                                      const AnalysisConfig&     config = {})
{
    AnalysisStage stage(config);
    stage.set_source_sample_rate(kAnalysisRate);

    Collector           c;
    constexpr std::size_t kChunk = 4096;
    const std::size_t     frames = interleaved.size() / 2;

    for (std::size_t at = 0; at < frames; at += kChunk) {
        const std::size_t n = std::min(kChunk, frames - at);
        stage.push(interleaved.data() + at * 2, n, 2, collect, &c);
    }
    return c.rows;
}

// ---------------------------------------------------------------------------
// Comparison.
// ---------------------------------------------------------------------------

// Columns whose value is a count or a flag. Compared EXACTLY: a beat that did
// not happen is not a small numerical difference.
bool is_exact_column(const std::string& name)
{
    return name == "frame_index" || name == "onset" || name == "onset_count" ||
           name == "beat_count";
}

// Wide enough for MSVC and gcc to disagree in, narrow enough that no deliberate
// analysis change fits inside it. See the header comment.
constexpr double kRelativeTolerance = 5e-4;
constexpr double kAbsoluteFloor     = 2e-6;   // the file only carries six decimals

std::vector<std::string> split(const std::string& line)
{
    std::vector<std::string> out;
    std::string              cell;
    std::istringstream       in(line);
    while (std::getline(in, cell, ',')) {
        while (!cell.empty() && (cell.back() == '\r' || cell.back() == '\n')) {
            cell.pop_back();
        }
        out.push_back(cell);
    }
    return out;
}

// Empty when they agree; otherwise the first disagreement, named.
std::string diff_against_golden(const std::vector<std::string>& produced,
                                const std::string&              golden_text,
                                double                          relative_tolerance)
{
    std::vector<std::string> golden_lines;
    {
        std::istringstream in(golden_text);
        std::string        line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (!line.empty()) {
                golden_lines.push_back(line);
            }
        }
    }

    if (golden_lines.empty()) {
        return "the golden file is empty";
    }

    const std::vector<std::string> columns = split(golden_lines[0]);

    std::string expected_header = frame_csv_header();
    if (!expected_header.empty() && expected_header.back() == '\n') {
        expected_header.pop_back();
    }
    if (golden_lines[0] != expected_header) {
        return "the golden file's columns are not the ones frame_csv_header() writes -- "
               "the format changed and the golden was not regenerated";
    }

    if (golden_lines.size() - 1 != produced.size()) {
        return "the golden has " + std::to_string(golden_lines.size() - 1) +
               " frames and the analysis produced " + std::to_string(produced.size());
    }

    for (std::size_t r = 0; r < produced.size(); ++r) {
        const std::vector<std::string> got  = split(produced[r]);
        const std::vector<std::string> want = split(golden_lines[r + 1]);

        if (got.size() != want.size() || got.size() != columns.size()) {
            return "row " + std::to_string(r) + " has the wrong number of columns";
        }

        for (std::size_t c = 0; c < got.size(); ++c) {
            if (got[c] == want[c]) {
                continue;
            }

            const std::string where =
                "row " + std::to_string(r) + ", column '" + columns[c] + "': golden " + want[c] +
                ", got " + got[c];

            if (is_exact_column(columns[c])) {
                return where + " (this column is compared exactly)";
            }

            const double a = std::strtod(got[c].c_str(), nullptr);
            const double b = std::strtod(want[c].c_str(), nullptr);
            if (!std::isfinite(a) || !std::isfinite(b)) {
                return where + " (non-finite)";
            }

            const double allowed =
                kAbsoluteFloor + relative_tolerance * std::max(std::fabs(a), std::fabs(b));
            if (std::fabs(a - b) > allowed) {
                char detail[128];
                std::snprintf(detail, sizeof(detail), " (differs by %.3g, tolerance %.3g)",
                              std::fabs(a - b), allowed);
                return where + detail;
            }
        }
    }
    return {};
}

std::string read_file(const char* path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return {};
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

// Same platform split as tests/test_projectm_api.cpp, and for the same reason:
// getenv is deprecated under MSVC's secure-CRT warnings and we build /WX.
bool writing_golden()
{
    std::string v;
#if defined(_WIN32)
    char*       value = nullptr;
    std::size_t len   = 0;
    if (_dupenv_s(&value, &len, "HOLOCRON_WRITE_GOLDEN") == 0 && value != nullptr) {
        v = value;
        std::free(value);
    }
#else
    const char* value = std::getenv("HOLOCRON_WRITE_GOLDEN");
    if (value != nullptr) {
        v = value;
    }
#endif
    return !v.empty() && v != "0";
}

void write_golden(const std::vector<std::string>& rows)
{
    // BINARY, so the '\n' the formatter writes is the byte that lands. Opened as
    // text on Windows every line would gain a CR, the committed file would be
    // CRLF against a .gitattributes that says LF, and `git diff` would show the
    // whole file as changed the next time anyone regenerated it on Linux.
    std::ofstream out(golden_path(), std::ios::binary | std::ios::trunc);
    REQUIRE(out.is_open());
    out << frame_csv_header();
    for (const std::string& row : rows) {
        out << row;
    }
    out.close();
    std::printf("\nwrote %s (%zu frames)\n", golden_path(), rows.size());
}

// ---------------------------------------------------------------------------
// The WAV path, for the second case.
// ---------------------------------------------------------------------------

void put32(std::ofstream& o, std::uint32_t v) { o.write(reinterpret_cast<const char*>(&v), 4); }
void put16(std::ofstream& o, std::uint16_t v) { o.write(reinterpret_cast<const char*>(&v), 2); }

void write_wav(const std::string& path, const std::vector<float>& interleaved)
{
    std::ofstream o(path, std::ios::binary);
    REQUIRE(o.good());

    const std::uint32_t data_size = std::uint32_t(interleaved.size()) * 2;

    o.write("RIFF", 4);
    put32(o, 36 + data_size);
    o.write("WAVE", 4);
    o.write("fmt ", 4);
    put32(o, 16);
    put16(o, 1);   // PCM
    put16(o, 2);   // stereo
    put32(o, kAnalysisRate);
    put32(o, kAnalysisRate * 4);
    put16(o, 4);
    put16(o, 16);
    o.write("data", 4);
    put32(o, data_size);

    // 32768 HERE, WHERE test_decoder.cpp's write_wav USES 32767, and the
    // difference is deliberate. That one takes arbitrary floats and wants the
    // loudest representable sample; this one takes samples `quantise` has
    // already put on the 16-bit grid as n/32768, so multiplying by 32768
    // recovers n exactly. Using 32767 here would round some samples to a
    // neighbouring integer and the two paths would no longer be the same audio.
    for (float v : interleaved) {
        const auto s = std::int16_t(std::lround(double(v) * 32768.0));
        o.write(reinterpret_cast<const char*>(&s), 2);
    }
    o.close();
}

struct ScopedFile {
    explicit ScopedFile(const char* stem)
        : path((std::filesystem::temp_directory_path() /
                (std::string("holocron_golden_") + stem + ".wav"))
                   .string())
    {
    }
    ~ScopedFile()
    {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    std::string path;
};

}  // namespace

// ---------------------------------------------------------------------------

TEST_CASE("analysis output matches the golden file", "[analysis][golden]")
{
    const std::vector<std::string> rows = run_analysis(fixture_signal());

    // 8 seconds at one frame per 512 samples of 48 kHz.
    REQUIRE(rows.size() == std::size_t(kFixtureSeconds * double(kAnalysisRate)) / kHopSize);

    if (writing_golden()) {
        write_golden(rows);
        SUCCEED("golden regenerated");
        return;
    }

    const std::string golden = read_file(golden_path());
    INFO("the golden file is " << golden_path());
    REQUIRE_FALSE(golden.empty());

    const std::string difference = diff_against_golden(rows, golden, kRelativeTolerance);
    INFO("if the analysis changed on purpose, regenerate with "
         "HOLOCRON_WRITE_GOLDEN=1 and commit the new file with the change");
    CHECK(difference.empty());
    if (!difference.empty()) {
        FAIL_CHECK(difference);
    }
}

// The criterion says the analysis runs "headless against a fixture", and the
// headless path is holocron-analyze: a file on disk, through the decoder and the
// tap resampler, into the stage. The case above skips both so that a broken
// decoder cannot mask an analysis regression. This one puts them back, against
// the same golden, so a broken decoder cannot hide either.
//
// It is allowed to agree EXACTLY rather than approximately -- 48 kHz stereo in
// and 48 kHz stereo out is a copy through swresample, and 16-bit to float is
// exact -- but it is compared with the same tolerance as everything else. The
// day vcpkg moves FFmpeg to a version that resamples differently, this is the
// test that says so.
TEST_CASE("the decoded path reproduces the golden file", "[analysis][golden][decoder]")
{
    // SKIPPED WHILE REGENERATING, because ctest runs each case as its own
    // process and does not promise an order. Comparing against a golden the
    // writer above has not written yet would fail for a reason that is purely
    // about scheduling.
    if (writing_golden()) {
        SUCCEED("skipped while regenerating the golden");
        return;
    }

    const std::vector<float> signal = fixture_signal();

    ScopedFile wav("fixture");
    write_wav(wav.path, signal);

    Decoder decoder;
    REQUIRE(decoder.open(wav.path.c_str()) == DecoderError::kOk);

    const SourceInfo info = decoder.info();
    CHECK(info.sample_rate == kAnalysisRate);
    CHECK(info.channels == 2);

    Resampler resampler;
    REQUIRE(resampler.configure(info.sample_rate, info.channels) == DecoderError::kOk);

    AnalysisStage stage;
    stage.set_source_sample_rate(info.sample_rate);

    Collector             c;
    constexpr std::size_t kChunk = 4096;
    std::vector<float>    native(kChunk * std::size_t(info.channels));
    std::vector<float>    tapped(resampler.max_output_frames(kChunk) * 2 + 64);

    while (true) {
        const std::size_t got = decoder.read(native.data(), kChunk);
        if (got == 0) {
            break;
        }
        const std::size_t out =
            resampler.process(native.data(), got, tapped.data(), tapped.size() / 2);
        if (out > 0) {
            stage.push(tapped.data(), out, 2, collect, &c);
        }
    }
    const std::size_t tail = resampler.flush(tapped.data(), tapped.size() / 2);
    if (tail > 0) {
        stage.push(tapped.data(), tail, 2, collect, &c);
    }

    const std::string golden = read_file(golden_path());
    REQUIRE_FALSE(golden.empty());

    // The decoder may deliver a few samples more or fewer than were written --
    // a container's frame count is not a promise about the last partial hop --
    // so the golden's rows are the ones compared and any extra tail is dropped.
    std::vector<std::string> rows = c.rows;
    const std::size_t        want = std::size_t(kFixtureSeconds * double(kAnalysisRate)) / kHopSize;
    CHECK(rows.size() >= want);
    rows.resize(std::min(rows.size(), want));

    const std::string difference = diff_against_golden(rows, golden, kRelativeTolerance);
    CHECK(difference.empty());
    if (!difference.empty()) {
        FAIL_CHECK(difference);
    }
}

// A GOLDEN FILE THAT CANNOT FAIL IS A FILE, NOT A TEST.
//
// The tolerance above is the whole risk in this design: set it slightly too wide
// and the comparison passes through every real change while looking like
// coverage. Nothing in the two cases above would notice, because both are
// supposed to pass.
//
// So this runs the same fixture through a config with ONE constant moved --
// band_decay from 0.25 s to 0.26 s, a 4% change to one envelope, far smaller
// than any analysis change anyone would make deliberately -- and requires the
// comparison to reject it. If the tolerance is ever widened past the point of
// usefulness, this is what goes red.
TEST_CASE("the golden comparison can fail", "[analysis][golden]")
{
    if (writing_golden()) {
        SUCCEED("skipped while regenerating the golden");
        return;
    }

    const std::string golden = read_file(golden_path());
    REQUIRE_FALSE(golden.empty());

    AnalysisConfig perturbed;
    perturbed.band_decay = 0.26f;

    const std::vector<std::string> rows = run_analysis(fixture_signal(), perturbed);

    const std::string difference = diff_against_golden(rows, golden, kRelativeTolerance);
    INFO("a 4% change to one envelope constant slipped through the tolerance");
    CHECK_FALSE(difference.empty());
}
