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
// JPEG WORKS. PNG DEPENDS ON THE FFmpeg BUILD, AND IN THIS ONE IT DOES NOT.
//
// PNG is DEFLATE-compressed, so its decoder needs zlib, and vcpkg.json takes
// FFmpeg with default-features off for licence reasons -- which drops zlib along
// with everything else. `avcodec_find_decoder(AV_CODEC_ID_PNG)` therefore returns
// nullptr here and a PNG is refused with kNoDecoder.
//
// That is acceptable rather than merely tolerated, because Holocron fetches art
// from Plex through the photo transcoder, which returns JPEG. The PNG path is
// kept because it costs nothing and starts working the day zlib is present, and
// it is REFUSED CLEANLY rather than misdecoded -- which is the property that
// matters, since the alternative is a sleeve of noise feeding a palette.
//
// Embedded art in a local file may well be PNG, so this becomes a real
// limitation the moment art is read from anywhere but Plex.
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
ImageError decode_image(const std::vector<std::uint8_t>& bytes, ImageRgba8& out,
                        std::string& out_detail);

}  // namespace holocron
