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
// PNG is DEFLATE-compressed, so its decoder needs zlib. This FFmpeg is built
// `--disable-zlib`, so `avcodec_find_decoder(AV_CODEC_ID_PNG)` returns nullptr
// here and a PNG is refused with kNoDecoder. The mechanism is in FFmpeg's own
// configure: `png_decoder_select="inflate_wrapper"`, `inflate_wrapper_deps="zlib"`.
//
// NOT BECAUSE OF `default-features: false`, which is what this comment claimed
// until 2026-08-13 and what three other files repeated. zlib is not among the
// vcpkg ffmpeg port's default features at the pinned baseline either, so turning
// them back on would pull in avdevice, avfilter and swscale and STILL leave PNG
// refused. The correction matters because the wrong cause suggests a fix that
// does nothing.
//
// AND IT WAS NOT MOOT. This comment used to say the PNG path was kept only
// against the day zlib arrived, on the grounds that Plex's photo transcoder
// returns JPEG. Measured over every album on the reference library -- see
// docs/measurements.toml, `artwork_png` -- it does not. `/photo/:/transcode`
// RESIZES and passes the source format through, so a sleeve stored as a PNG
// arrived as PNG, was refused here, and took the palette down with it. It also
// labelled every one of those responses `image/jpeg`, which is why `sniff()`
// below reads the bytes instead of the header.
//
// THE FIX IS IN THE REQUEST, NOT HERE: `artwork_path()` in plex_playback.cpp now
// asks for `format=jpeg` and the transcoder obliges. So this path is genuinely
// unreached for Plex art again -- but it is unreached because something asks for
// the right thing, not because the server was ever going to do it unprompted. A
// server too old to know `format` would fall back to the old behaviour silently,
// which is what the kNoDecoder detail below and the caller's log line are for.
//
// Refusing CLEANLY rather than misdecoding is still the property that matters:
// the alternative is a sleeve of noise feeding a palette, producing confident
// wrong colours with no error anywhere.
//
// IF zlib IS EVER ADDED, THAT IS NOT THE WHOLE JOB. `packed_to_rgba` handles four
// layouts -- RGB24, RGBA, RGB0, GRAY8 -- and FFmpeg's pngdec can emit ten, the
// extras being PAL8, MONOBLACK, YA8, YA16BE, GRAY16BE, RGB48BE and RGBA64BE.
// Indexed PNG is ordinary for simple cover art, so zlib alone converts "PNG is
// always refused" into "PNG is sometimes refused for a different reason". Every
// PNG in the census happened to be RGB24 or RGBA, so this library would have been
// fine; another one need not be.
//
// Embedded art in a local file may well be PNG, and there is no request to add a
// parameter to, so that path would need zlib for real.
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
