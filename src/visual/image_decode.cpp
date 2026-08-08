// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// src/visual/image_decode.cpp
//
// See image_decode.hpp for why there is no swscale in here.

#include <holocron/image_decode.hpp>

#include <algorithm>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}

namespace holocron {
namespace {

// A sleeve larger than this is refused rather than decoded.
//
// The bytes arrive from the network, and `width * height * 4` is an allocation
// sized by something the player did not choose. Plex thumbs are a few hundred
// pixels square; 8192 is far above anything real and still bounds the buffer at
// 256 MB rather than at whatever a malformed header claims.
constexpr int kMaxDimension = 8192;

// Clamp to a byte, which is what every conversion below needs and what C++ does
// not provide for the float-to-uint8 case.
std::uint8_t clamp_byte(float v)
{
    if (v <= 0.0f) {
        return 0;
    }
    if (v >= 255.0f) {
        return 255;
    }
    return static_cast<std::uint8_t>(v + 0.5f);
}

// What the magic bytes say this is.
//
// The content type from the server is NOT trusted for this. Plex serves
// thumbnails through a transcoder that has been observed to label a JPEG as
// image/png, and the decoder picked from a wrong label fails in a way that reads
// as a corrupt image rather than as a mislabelled one.
AVCodecID sniff(const std::vector<std::uint8_t>& bytes)
{
    if (bytes.size() >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF) {
        return AV_CODEC_ID_MJPEG;
    }
    if (bytes.size() >= 8 && bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N' &&
        bytes[3] == 'G' && bytes[4] == 0x0D && bytes[5] == 0x0A && bytes[6] == 0x1A &&
        bytes[7] == 0x0A) {
        return AV_CODEC_ID_PNG;
    }
    return AV_CODEC_ID_NONE;
}

// -- the conversions ---------------------------------------------------------
//
// BT.601 coefficients, which is what JPEG specifies and what every album
// thumbnail in existence was encoded with. NOT BT.709: using the HD matrix on a
// JPEG shifts every hue slightly, which looks like nothing at all until a
// palette built from it is compared against the sleeve.
//
// Two ranges, and the difference is not cosmetic. AV_PIX_FMT_YUVJ* is FULL
// range -- luma spans 0..255 -- and is what a JPEG decodes to. AV_PIX_FMT_YUV*
// is limited range, luma 16..235. Treating limited as full washes the image out;
// treating full as limited crushes the blacks and blows the highlights.

struct YuvGeometry {
    int  chroma_shift_x = 1;
    int  chroma_shift_y = 1;
    bool full_range     = true;
    bool supported      = false;
};

YuvGeometry geometry_of(AVPixelFormat format)
{
    switch (format) {
        case AV_PIX_FMT_YUVJ420P: return {1, 1, true, true};
        case AV_PIX_FMT_YUVJ422P: return {1, 0, true, true};
        case AV_PIX_FMT_YUVJ444P: return {0, 0, true, true};
        case AV_PIX_FMT_YUV420P:  return {1, 1, false, true};
        case AV_PIX_FMT_YUV422P:  return {1, 0, false, true};
        case AV_PIX_FMT_YUV444P:  return {0, 0, false, true};
        default:                  return {};
    }
}

void yuv_to_rgba(const AVFrame& frame, const YuvGeometry& geo, ImageRgba8& out)
{
    for (int y = 0; y < out.height; ++y) {
        const std::uint8_t* luma = frame.data[0] + static_cast<std::ptrdiff_t>(y) * frame.linesize[0];
        const int           cy   = y >> geo.chroma_shift_y;
        const std::uint8_t* cb   = frame.data[1] + static_cast<std::ptrdiff_t>(cy) * frame.linesize[1];
        const std::uint8_t* cr   = frame.data[2] + static_cast<std::ptrdiff_t>(cy) * frame.linesize[2];

        std::uint8_t* row = out.pixels.data() +
                            static_cast<std::size_t>(y) * static_cast<std::size_t>(out.width) * 4;

        for (int x = 0; x < out.width; ++x) {
            const int cx = x >> geo.chroma_shift_x;

            float yv = static_cast<float>(luma[x]);
            if (!geo.full_range) {
                // 16..235 to 0..255. Clamping first, because limited-range
                // content legally contains values outside the nominal range and
                // the arithmetic below would take them negative.
                yv = (std::clamp(yv, 16.0f, 235.0f) - 16.0f) * (255.0f / 219.0f);
            }

            // Chroma is centred on 128 in both ranges; only its excursion
            // differs, and the 224 vs 255 span is folded into the coefficients
            // for the limited case.
            const float u = static_cast<float>(cb[cx]) - 128.0f;
            const float v = static_cast<float>(cr[cx]) - 128.0f;
            const float scale = geo.full_range ? 1.0f : (255.0f / 224.0f);

            const float su = u * scale;
            const float sv = v * scale;

            row[x * 4 + 0] = clamp_byte(yv + 1.402f * sv);
            row[x * 4 + 1] = clamp_byte(yv - 0.344136f * su - 0.714136f * sv);
            row[x * 4 + 2] = clamp_byte(yv + 1.772f * su);
            row[x * 4 + 3] = 255;
        }
    }
}

// PNG and the other packed layouts. Straight repacking, no colour maths.
bool packed_to_rgba(const AVFrame& frame, AVPixelFormat format, ImageRgba8& out)
{
    for (int y = 0; y < out.height; ++y) {
        const std::uint8_t* src = frame.data[0] + static_cast<std::ptrdiff_t>(y) * frame.linesize[0];
        std::uint8_t*       row = out.pixels.data() +
                            static_cast<std::size_t>(y) * static_cast<std::size_t>(out.width) * 4;

        switch (format) {
            case AV_PIX_FMT_RGB24:
                for (int x = 0; x < out.width; ++x) {
                    row[x * 4 + 0] = src[x * 3 + 0];
                    row[x * 4 + 1] = src[x * 3 + 1];
                    row[x * 4 + 2] = src[x * 3 + 2];
                    row[x * 4 + 3] = 255;
                }
                break;

            case AV_PIX_FMT_RGBA:
                std::memcpy(row, src, static_cast<std::size_t>(out.width) * 4);
                break;

            case AV_PIX_FMT_RGB0:
                for (int x = 0; x < out.width; ++x) {
                    row[x * 4 + 0] = src[x * 4 + 0];
                    row[x * 4 + 1] = src[x * 4 + 1];
                    row[x * 4 + 2] = src[x * 4 + 2];
                    // The fourth byte is PADDING, not alpha. Copying it through
                    // gives a sleeve that is entirely transparent, which the
                    // palette then reads as an image of nothing and answers with
                    // the neutral ramp -- a missing palette with no error.
                    row[x * 4 + 3] = 255;
                }
                break;

            case AV_PIX_FMT_GRAY8:
                for (int x = 0; x < out.width; ++x) {
                    row[x * 4 + 0] = src[x];
                    row[x * 4 + 1] = src[x];
                    row[x * 4 + 2] = src[x];
                    row[x * 4 + 3] = 255;
                }
                break;

            default:
                return false;
        }
    }
    return true;
}

// RAII, because there are five failure paths below and every one of them has to
// release the context and the frame.
struct Decoding {
    AVCodecContext* context = nullptr;
    AVFrame*        frame   = nullptr;
    AVPacket*       packet  = nullptr;

    ~Decoding()
    {
        if (packet != nullptr) {
            av_packet_free(&packet);
        }
        if (frame != nullptr) {
            av_frame_free(&frame);
        }
        if (context != nullptr) {
            avcodec_free_context(&context);
        }
    }
};

}  // namespace

const char* to_string(ImageError e)
{
    switch (e) {
        case ImageError::kOk:                     return "ok";
        case ImageError::kEmpty:                  return "there were no bytes to decode";
        case ImageError::kUnknownFormat:          return "not a JPEG or a PNG";
        case ImageError::kNoDecoder:              return "FFmpeg has no decoder for this image";
        case ImageError::kBadImage:               return "the image would not decode";
        case ImageError::kUnsupportedPixelFormat: return "the image is in a pixel layout "
                                                         "holocron does not convert";
    }
    return "unknown";
}

ImageError decode_image(const std::vector<std::uint8_t>& bytes, ImageRgba8& out,
                        std::string& out_detail)
{
    out_detail.clear();

    if (bytes.empty()) {
        return ImageError::kEmpty;
    }

    const AVCodecID id = sniff(bytes);
    if (id == AV_CODEC_ID_NONE) {
        return ImageError::kUnknownFormat;
    }

    const AVCodec* codec = avcodec_find_decoder(id);
    if (codec == nullptr) {
        return ImageError::kNoDecoder;
    }

    Decoding d;
    d.context = avcodec_alloc_context3(codec);
    d.frame   = av_frame_alloc();
    d.packet  = av_packet_alloc();
    if (d.context == nullptr || d.frame == nullptr || d.packet == nullptr) {
        return ImageError::kBadImage;
    }

    if (avcodec_open2(d.context, codec, nullptr) < 0) {
        return ImageError::kBadImage;
    }

    // The whole file as one packet. A JPEG and a PNG are each a single coded
    // picture, so there is nothing to parse apart and no container to demux.
    //
    // AV_INPUT_BUFFER_PADDING_SIZE of zeroed slack past the end is REQUIRED by
    // the bitstream readers, which are allowed to read a few bytes beyond what
    // they use. Without it this decodes correctly almost always and reads off
    // the end of the allocation occasionally.
    std::vector<std::uint8_t> padded(bytes.size() + AV_INPUT_BUFFER_PADDING_SIZE, 0);
    std::memcpy(padded.data(), bytes.data(), bytes.size());

    d.packet->data = padded.data();
    d.packet->size = static_cast<int>(bytes.size());

    if (avcodec_send_packet(d.context, d.packet) < 0) {
        return ImageError::kBadImage;
    }
    if (avcodec_receive_frame(d.context, d.frame) < 0) {
        return ImageError::kBadImage;
    }

    const int width  = d.frame->width;
    const int height = d.frame->height;
    if (width <= 0 || height <= 0 || width > kMaxDimension || height > kMaxDimension) {
        out_detail = "the image claims a size holocron will not allocate for";
        return ImageError::kBadImage;
    }

    const auto format = static_cast<AVPixelFormat>(d.frame->format);

    ImageRgba8 decoded;
    decoded.width  = width;
    decoded.height = height;
    decoded.pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);

    const YuvGeometry geo = geometry_of(format);
    if (geo.supported) {
        yuv_to_rgba(*d.frame, geo, decoded);
    } else if (!packed_to_rgba(*d.frame, format, decoded)) {
        const char* name = av_get_pix_fmt_name(format);
        out_detail       = name != nullptr ? name : "unnamed pixel format";
        return ImageError::kUnsupportedPixelFormat;
    }

    out = std::move(decoded);
    return ImageError::kOk;
}

}  // namespace holocron
