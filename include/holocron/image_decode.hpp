// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/image_decode.hpp
//
// Turn the bytes of an album sleeve into RGBA8.
//
// ONE JOB, AND IT IS NOT A GENERAL IMAGE LIBRARY. The only images this project
// decodes are Plex thumbnails: JPEG in practice, PNG occasionally, a few hundred
// pixels square, one per track. Everything here is sized for that and would be
// the wrong shape for anything else.
//
// WHY THERE IS NO swscale
//
// vcpkg.json takes FFmpeg with an explicit feature list -- avcodec, avformat,
// swresample -- and says in as many words that avdevice, avfilter and swscale
// are dropped because an audio decoder does not use them. Adding swscale to
// convert one thumbnail per track would take a dependency for a job that is
// twenty lines of arithmetic, and would invalidate the vcpkg cache, which costs
// roughly twenty-five minutes of Windows CI on the change that does it and
// nothing thereafter. The conversion is written out below instead. Decision
// confirmed with the owner 2026-08-08.
//
// The formats handled are the ones a Plex thumb actually arrives in. Anything
// else is reported as unsupported rather than guessed at: a wrong pixel format
// silently produces a sleeve with its colours swapped, and a palette built from
// that is wrong in a way nothing downstream can detect.
//
// JPEG AND PNG BOTH WORK. PNG TOOK THREE GOES AND THE HISTORY IS LOAD-BEARING.
//
// PNG is DEFLATE-compressed, so its decoder needs zlib, and this FFmpeg did not
// have it until issue 116 was finished. `vcpkg.json` now asks for ffmpeg's
// `zlib` feature unconditionally, so `avcodec_find_decoder(AV_CODEC_ID_PNG)`
// succeeds on every platform. The mechanism is in FFmpeg's own configure:
// `png_decoder_select="inflate_wrapper"`, `inflate_wrapper_deps="zlib"`.
//
// Two wrong explanations were believed along the way, and both are worth keeping
// because each suggested a fix that would have done nothing:
//
// 1. NOT `default-features: false`. Four files said so. zlib is not among the
//    vcpkg ffmpeg port's defaults either, so turning them back on would have
//    pulled in avdevice, avfilter and swscale and STILL left PNG refused.
//
// 2. NOT MOOT BECAUSE "Plex returns JPEG". Measured over every album on the
//    reference library -- docs/measurements.toml, `artwork_png` --
//    `/photo/:/transcode` RESIZES and passes the source format straight through,
//    so 157 of 2,450 sleeves arrived as PNG and took the palette down with them.
//    It labels every one of them `image/jpeg` regardless, which is why `sniff()`
//    below reads the BYTES rather than the header, and is the only reason this
//    failed cleanly instead of misdecoding.
//
// TWO FIXES, FOR TWO DIFFERENT CALLERS, AND BOTH ARE NEEDED.
//
// Plex art is FETCHED, so the cheap fix is in the request: `artwork_path()` in
// plex_playback.cpp asks for `format=jpeg` and the transcoder obliges. That
// shipped first and still stands -- it avoids a decode rather than relying on
// one, so a sleeve is smaller over the wire and the path is exercised constantly.
//
// LOCAL FILES HAVE NO REQUEST TO ADD A PARAMETER TO. `--art PATH` reads a file
// off disk, and it is the instrument every palette question now goes through. It
// is why zlib was added for real rather than left as a recipe: a PNG on disk had
// no way round this at all.
//
// zlib ALONE IS NOT THE WHOLE JOB, WHICH IS THE PART THAT LOOKS DONE AND IS NOT.
// FFmpeg's pngdec can emit TEN pixel layouts. `packed_to_rgba` handles RGB24,
// RGBA, RGB0, GRAY8 and PAL8; the five it refuses by name are MONOBLACK, YA8,
// YA16BE, GRAY16BE, RGB48BE and RGBA64BE. PAL8 was added with zlib because
// INDEXED PNG IS ORDINARY for flat-colour cover art -- without it, zlib would
// have converted "PNG is always refused" into "PNG is sometimes refused for a
// different reason". Every PNG in the census happened to be RGB24 or RGBA, so
// this library would have been fine either way; another one need not be.
//
// avformat IS NOT USED EITHER. A JPEG or a PNG is fed to the decoder as a single
// packet -- there is no container to demux, and reaching for avformat would mean
// a custom AVIO buffer for no benefit.

#pragma once

#include <holocron/palette.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace holocron {

enum class ImageError : std::uint8_t {
    kOk = 0,

    kEmpty,          // nothing to decode
    kUnknownFormat,  // not a JPEG or a PNG
    kNoDecoder,      // FFmpeg was built without the codec
    kBadImage,       // the decoder rejected the bytes
    kUnsupportedPixelFormat,  // decoded, but in a layout not handled here
};

const char* to_string(ImageError e);

// Decode `bytes` into `out`.
//
// On anything but kOk, `out` is left untouched and the caller should carry on
// with no art -- a sleeve that will not decode is a cosmetic loss, never a
// reason to interrupt playback.
//
// `out_detail` IS FILLED ON EVERY NON-kOk RETURN, and cleared on kOk so a caller
// reusing one string across a fetch and a decode cannot print last track's
// reason. The enum names the class of failure, the detail names this instance;
// log both, in that order. Three of the seven exits used to leave it blank, which
// is how issue 116 stayed invisible for as long as it did.
ImageError decode_image(const std::vector<std::uint8_t>& bytes, ImageRgba8& out,
                        std::string& out_detail);

}  // namespace holocron
