// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/decoder.hpp
//
// FFmpeg-backed decoding, and the resampler that feeds the analysis tap.
//
// TWO RATES, AND CONFLATING THEM IS THE BUG THIS FILE EXISTS TO PREVENT.
//
//   * The OUTPUT path runs at the file's native rate and stays bit-perfect
//     where the platform allows it. Nothing here resamples it.
//   * The ANALYSIS tap is resampled to a fixed kAnalysisRate (48 kHz) so a
//     crystal behaves identically on a 44.1 kHz rip and a 192 kHz master.
//
// See D-004 and docs/audio-frame.md section 2. `Decoder` produces native-rate
// audio; `Resampler` converts it for the tap. They are separate types because
// the moment they are one type, someone will feed the resampled stream to the
// sink and quietly break the bit-perfect promise.
//
// FFmpeg is reached through this file and nowhere else.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace holocron {

// ---------------------------------------------------------------------------

enum class DecoderError : std::uint8_t {
    kOk = 0,

    kFileNotFound,      // the path does not exist
    kNotAudio,          // the file exists but is not decodable audio -- either
                        // not a recognised container at all, or a container
                        // with no audio stream. Distinct from kFileNotFound on
                        // purpose: "no such file" and "that is not audio" are
                        // different messages to a user.
    kUnsupportedCodec,  // audio stream found, no decoder for it
    kCorruptStream,
    kAlreadyOpen,
    kNotOpen,
    kBackendFailure,
};

constexpr const char* to_string(DecoderError e)
{
    switch (e) {
    case DecoderError::kOk:               return "ok";
    case DecoderError::kFileNotFound:     return "file not found";
    case DecoderError::kNotAudio:         return "no audio stream in file";
    case DecoderError::kUnsupportedCodec: return "no decoder for this codec";
    case DecoderError::kCorruptStream:    return "corrupt or truncated stream";
    case DecoderError::kAlreadyOpen:      return "already open";
    case DecoderError::kNotOpen:          return "not open";
    case DecoderError::kBackendFailure:   return "backend failure";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// What a file turned out to be. `sample_rate` here is what AudioFrame reports
// for display, and what the sink should be opened at -- never what the analysis
// arrays are in.
// ---------------------------------------------------------------------------

struct SourceInfo {
    std::uint32_t sample_rate      = 0;
    std::uint16_t channels         = 0;
    double        duration_seconds = 0.0;  // 0 if the container does not say
    const char*   codec_name       = "";
    bool          is_lossless      = false;

    // -- what the container says it IS ---------------------------------------
    //
    // Tags, and they are the only metadata a locally-played file has. TrackContext
    // is otherwise filled from the Plex path, which `holocron track.flac` never
    // reaches -- so before these existed the overlay had nothing but the filename
    // (issue 133).
    //
    // Empty when the container carries no such tag, which is common and is not an
    // error. A caller should fall back rather than report a problem.
    //
    // ALSO FILLS THE TWO FIELDS THE PLEX PATH CANNOT. `genre` and `year` are not
    // on a Plex Track element and would cost a second request per track, so a cast
    // leaves them empty -- a locally-played file gets them for free.
    //
    // UTF-8, AND THAT IS NOT FREE. Tag bytes are arbitrary: ID3v2.3 defaults to
    // Latin-1 and plenty of rippers write whatever the system codepage was.
    // Everything downstream assumes UTF-8 -- the text rasterizer calls
    // MultiByteToWideChar(CP_UTF8) and the control page declares it -- so
    // decoder.cpp validates and drops a tag it cannot vouch for rather than
    // passing mojibake along. See the note there.
    std::string title;
    std::string artist;
    std::string album;
    std::string genre;
    std::string year;
};

// Whether `text` is well-formed UTF-8.
//
// EXPOSED PURELY SO IT CAN BE TESTED, and that is worth the slightly odd shape of
// a decoder header exporting a string predicate.
//
// Tag bytes are arbitrary. ID3v2.3 defaults to Latin-1 and plenty of rippers write
// whatever the system codepage was; FFmpeg converts ID3 for the common cases and
// guarantees nothing for every container. Everything downstream assumes UTF-8 --
// the text rasterizer calls MultiByteToWideChar(CP_UTF8, ...) and the control page
// declares charset=utf-8 -- so a tag that fails this is DROPPED rather than passed
// on, and the caller falls back.
//
// The failure it prevents is mojibake, which produces no error, no crash and no
// wrong-looking data structure: just wrong characters on a screen. That is exactly
// the class of bug that needs a test rather than a careful reading.
bool is_valid_utf8(const char* text);

// ---------------------------------------------------------------------------
// Decoder: file in, interleaved float at the file's NATIVE rate out.
//
// Float rather than the source's integer format because everything downstream
// works in float and the conversion has to happen somewhere. Whether a
// bit-exact integer passthrough is needed for the output path is issue #36 --
// this is the analysis-side decoder and that question does not arise here.
// ---------------------------------------------------------------------------

class Decoder {
public:
    Decoder();
    ~Decoder();

    Decoder(const Decoder&)            = delete;
    Decoder& operator=(const Decoder&) = delete;

    DecoderError open(const char* path);
    void         close();
    bool         is_open() const;

    SourceInfo info() const;

    // Fill up to `max_frames` frames of interleaved float at the native rate
    // and channel count. Returns frames written; 0 means end of stream.
    std::size_t read(float* out, std::size_t max_frames);

    bool at_end() const;

    // Whether this source can be seeked at all.
    //
    // FALSE IS A REAL ANSWER, NOT AN EDGE CASE. A pipe, a live stream, or an
    // HTTP server that refuses range requests cannot seek, and the caller has to
    // reach the position by decoding forward instead. Asking first means that
    // fallback is a decision rather than an error path.
    bool can_seek() const;

    // Move to `position_seconds`, measured from the start of the stream.
    //
    // LANDS AT OR BEFORE the request, never after: seeking forward to the
    // nearest keyframe would skip audio the listener asked to hear. The caller
    // is expected to decode and discard the remainder if it needs to be exact,
    // which for audio is a few tens of milliseconds.
    //
    // Returns kBackendFailure when the seek was attempted and refused, and
    // kNotOpen with nothing open. On failure the read position is UNSPECIFIED --
    // a refused seek can still have moved the demuxer -- so a caller that cannot
    // seek should reopen rather than carry on reading.
    DecoderError seek(double position_seconds);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// Resampler: native rate/channels in, kAnalysisRate stereo out.
//
// This is the tap conversion and nothing else. It exists so the analysis stage
// can assume 48 kHz stereo unconditionally, which is what makes crystal
// behaviour identical across the library.
// ---------------------------------------------------------------------------

class Resampler {
public:
    Resampler();
    ~Resampler();

    Resampler(const Resampler&)            = delete;
    Resampler& operator=(const Resampler&) = delete;

    // Configure for a source format. Safe to call again to reconfigure; any
    // buffered samples are discarded.
    DecoderError configure(std::uint32_t source_rate, std::uint16_t source_channels);
    bool         is_configured() const;

    // Worst-case output frames for a given input, so callers can size buffers
    // without guessing.
    std::size_t max_output_frames(std::size_t input_frames) const;

    // Convert. Returns frames written to `out` (stereo interleaved).
    std::size_t process(const float* in, std::size_t input_frames,
                        float* out, std::size_t max_output);

    // Drain whatever the resampler is holding. Call at end of stream, or the
    // last few milliseconds of every track are silently lost.
    std::size_t flush(float* out, std::size_t max_output);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace holocron
