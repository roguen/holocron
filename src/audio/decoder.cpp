// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// FFmpeg-backed decoding. See include/holocron/decoder.hpp for the contract.
//
// FFmpeg is reached through this translation unit and nowhere else, so the
// rest of the project never sees an AVFrame.

#include <holocron/decoder.hpp>

#include <holocron/audio_frame.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

namespace holocron {
namespace {

bool codec_is_lossless(AVCodecID id)
{
    switch (id) {
    case AV_CODEC_ID_FLAC:
    case AV_CODEC_ID_ALAC:
    case AV_CODEC_ID_APE:
    case AV_CODEC_ID_WAVPACK:
    case AV_CODEC_ID_TTA:
    case AV_CODEC_ID_TRUEHD:
    case AV_CODEC_ID_MLP:
    case AV_CODEC_ID_PCM_S16LE:
    case AV_CODEC_ID_PCM_S24LE:
    case AV_CODEC_ID_PCM_S32LE:
    case AV_CODEC_ID_PCM_F32LE:
        return true;
    default:
        return false;
    }
}

}  // namespace

// See decoder.hpp for why this is public and what it prevents.
//
// TAG BYTES ARE ARBITRARY AND THIS IS NOT PARANOIA. ID3v2.3 defaults to Latin-1
// and plenty of rippers write whatever the system codepage happened to be.
// FFmpeg converts ID3 for the common cases and does not guarantee it for every
// container.
//
// DROPPING A TAG WE CANNOT VOUCH FOR beats guessing at its encoding. Guessing
// means a heuristic that is wrong sometimes and produces confidently mangled
// text; dropping means the caller falls back to the filename, which is honest.
bool is_valid_utf8(const char* text)
{
    if (text == nullptr) {
        return false;
    }

    const auto* p = reinterpret_cast<const unsigned char*>(text);
    while (*p != 0) {
        int len = 0;
        if (*p < 0x80) {
            len = 1;
        } else if ((*p & 0xE0) == 0xC0) {
            len = 2;
        } else if ((*p & 0xF0) == 0xE0) {
            len = 3;
        } else if ((*p & 0xF8) == 0xF0) {
            len = 4;
        } else {
            return false;   // a continuation byte or an invalid lead
        }

        // Overlong two-byte forms encode ASCII in two bytes, which some encoders
        // emit and which is not valid UTF-8.
        if (len == 2 && (*p & 0xFE) == 0xC0) {
            return false;
        }

        for (int i = 1; i < len; ++i) {
            if ((p[i] & 0xC0) != 0x80) {
                return false;
            }
        }
        p += len;
    }
    return true;
}

namespace {

// Copy one tag if the container has it and it is not already set.
//
// FIRST WRITER WINS, which is why the container dictionary is read before the
// stream's: the container's is the one a tagger normally writes, and the stream's
// is the fallback for formats that put them there instead.
void take_tag(const AVDictionary* tags, const char* key, std::string& out)
{
    if (!out.empty()) {
        return;
    }
    const AVDictionaryEntry* e = av_dict_get(tags, key, nullptr, 0);
    if (e == nullptr || e->value == nullptr || e->value[0] == 0) {
        return;
    }
    if (!is_valid_utf8(e->value)) {
        return;
    }
    out = e->value;
}

void read_tags(const AVDictionary* tags, SourceInfo& info)
{
    if (tags == nullptr) {
        return;
    }

    take_tag(tags, "title", info.title);
    take_tag(tags, "artist", info.artist);
    take_tag(tags, "album", info.album);
    take_tag(tags, "genre", info.genre);

    // `date` is what most containers use; Vorbis comments in particular. `year`
    // is the older ID3 spelling and still turns up, so both are tried.
    take_tag(tags, "date", info.year);
    take_tag(tags, "year", info.year);

    // Trimmed to the year. A `date` tag is frequently a full ISO timestamp, and
    // "2001-05-15T00:00:00Z" on a now-playing card is noise.
    if (info.year.size() > 4) {
        const std::string head = info.year.substr(0, 4);
        if (head.find_first_not_of("0123456789") == std::string::npos) {
            info.year = head;
        }
    }
}

}  // namespace

// ===========================================================================
// Decoder
// ===========================================================================

struct Decoder::Impl {
    ~Impl() { teardown(); }

    void teardown()
    {
        if (swr != nullptr) {
            swr_free(&swr);
        }
        if (frame != nullptr) {
            av_frame_free(&frame);
        }
        if (packet != nullptr) {
            av_packet_free(&packet);
        }
        if (codec_ctx != nullptr) {
            avcodec_free_context(&codec_ctx);
        }
        if (format_ctx != nullptr) {
            avformat_close_input(&format_ctx);
        }
        pending.clear();
        pending_pos = 0;
        eof         = false;
        drained     = false;
        open_       = false;
    }

    // Pull one decoded AVFrame into `pending` as interleaved float.
    // Returns false when the stream is exhausted.
    bool decode_more();

    AVFormatContext* format_ctx = nullptr;
    AVCodecContext*  codec_ctx  = nullptr;
    AVPacket*        packet     = nullptr;
    AVFrame*         frame      = nullptr;
    SwrContext*      swr        = nullptr;  // format normalisation only, NOT rate conversion

    int  stream_index = -1;
    bool open_        = false;
    bool eof          = false;   // no more packets to read
    bool drained      = false;   // decoder flushed

    SourceInfo  info{};
    std::string codec_name_storage;

    std::vector<float> pending;
    std::size_t        pending_pos = 0;
};

bool Decoder::Impl::decode_more()
{
    while (true) {
        int ret = avcodec_receive_frame(codec_ctx, frame);

        if (ret == 0) {
            const int in_samples = frame->nb_samples;
            const int channels   = codec_ctx->ch_layout.nb_channels;
            if (in_samples <= 0 || channels <= 0) {
                continue;
            }

            const std::size_t base = pending.size();
            pending.resize(base + std::size_t(in_samples) * std::size_t(channels));

            auto*     dst      = reinterpret_cast<std::uint8_t*>(pending.data() + base);
            const int produced = swr_convert(swr, &dst, in_samples,
                                             const_cast<const std::uint8_t**>(frame->extended_data),
                                             in_samples);
            if (produced < 0) {
                pending.resize(base);
                return false;
            }
            pending.resize(base + std::size_t(produced) * std::size_t(channels));
            av_frame_unref(frame);
            return true;
        }

        if (ret == AVERROR_EOF) {
            return false;
        }

        if (ret != AVERROR(EAGAIN)) {
            return false;  // genuine decode error
        }

        // Decoder wants more input.
        if (eof) {
            if (drained) {
                return false;
            }
            avcodec_send_packet(codec_ctx, nullptr);  // flush
            drained = true;
            continue;
        }

        const int read_ret = av_read_frame(format_ctx, packet);
        if (read_ret < 0) {
            eof = true;
            continue;
        }

        if (packet->stream_index != stream_index) {
            av_packet_unref(packet);
            continue;
        }

        const int send_ret = avcodec_send_packet(codec_ctx, packet);
        av_packet_unref(packet);
        if (send_ret < 0 && send_ret != AVERROR(EAGAIN)) {
            return false;
        }
    }
}

// ---------------------------------------------------------------------------

Decoder::Decoder() : impl_(std::make_unique<Impl>()) {}
Decoder::~Decoder() = default;

DecoderError Decoder::open(const char* path)
{
    if (path == nullptr) {
        return DecoderError::kFileNotFound;
    }
    if (impl_->open_) {
        return DecoderError::kAlreadyOpen;
    }

    Impl& d = *impl_;

    int ret = avformat_open_input(&d.format_ctx, path, nullptr, nullptr);
    if (ret < 0) {
        d.teardown();
        // Map FFmpeg's reason rather than collapsing every failure into
        // "file not found". A caller needs to tell "no such file" from "that is
        // not a media file" -- they are different messages to a user, and
        // distinguishing them is the entire reason this returns an enum.
        if (ret == AVERROR(ENOENT)) {
            return DecoderError::kFileNotFound;
        }
        if (ret == AVERROR_INVALIDDATA) {
            return DecoderError::kNotAudio;
        }
        return DecoderError::kBackendFailure;
    }

    if (avformat_find_stream_info(d.format_ctx, nullptr) < 0) {
        d.teardown();
        return DecoderError::kCorruptStream;
    }

    d.stream_index = av_find_best_stream(d.format_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (d.stream_index < 0) {
        d.teardown();
        return DecoderError::kNotAudio;
    }

    AVStream* stream = d.format_ctx->streams[d.stream_index];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (codec == nullptr) {
        d.teardown();
        return DecoderError::kUnsupportedCodec;
    }

    d.codec_ctx = avcodec_alloc_context3(codec);
    if (d.codec_ctx == nullptr) {
        d.teardown();
        return DecoderError::kBackendFailure;
    }
    if (avcodec_parameters_to_context(d.codec_ctx, stream->codecpar) < 0) {
        d.teardown();
        return DecoderError::kBackendFailure;
    }
    if (avcodec_open2(d.codec_ctx, codec, nullptr) < 0) {
        d.teardown();
        return DecoderError::kUnsupportedCodec;
    }

    const int channels = d.codec_ctx->ch_layout.nb_channels;
    if (channels <= 0 || d.codec_ctx->sample_rate <= 0) {
        d.teardown();
        return DecoderError::kCorruptStream;
    }

    // Normalise sample FORMAT only -- same rate, same channel count. Planar
    // and integer formats become interleaved float; nothing is resampled here,
    // because the output path must stay at the native rate.
    ret = swr_alloc_set_opts2(&d.swr,
                              &d.codec_ctx->ch_layout, AV_SAMPLE_FMT_FLT, d.codec_ctx->sample_rate,
                              &d.codec_ctx->ch_layout, d.codec_ctx->sample_fmt, d.codec_ctx->sample_rate,
                              0, nullptr);
    if (ret < 0 || swr_init(d.swr) < 0) {
        d.teardown();
        return DecoderError::kBackendFailure;
    }

    d.packet = av_packet_alloc();
    d.frame  = av_frame_alloc();
    if (d.packet == nullptr || d.frame == nullptr) {
        d.teardown();
        return DecoderError::kBackendFailure;
    }

    d.codec_name_storage = (codec->name != nullptr) ? codec->name : "";

    d.info.sample_rate = std::uint32_t(d.codec_ctx->sample_rate);
    d.info.channels    = std::uint16_t(channels);
    d.info.codec_name  = d.codec_name_storage.c_str();
    d.info.is_lossless = codec_is_lossless(stream->codecpar->codec_id);
    d.info.duration_seconds =
        (d.format_ctx->duration > 0)
            ? double(d.format_ctx->duration) / double(AV_TIME_BASE)
            : 0.0;

    // Tags, from the container and then the stream.
    //
    // BOTH DICTIONARIES, IN THAT ORDER. Most formats put them on the container;
    // some -- notably Matroska and a few MP4 variants -- put them on the stream
    // instead, and a file that reads correctly in every other player would come
    // back untitled if only one were consulted.
    read_tags(d.format_ctx->metadata, d.info);
    read_tags(stream->metadata, d.info);

    d.open_ = true;
    return DecoderError::kOk;
}

void Decoder::close() { impl_->teardown(); }

bool Decoder::is_open() const { return impl_->open_; }

SourceInfo Decoder::info() const { return impl_->info; }

bool Decoder::at_end() const
{
    const Impl& d = *impl_;
    return d.open_ && d.eof && d.drained && d.pending_pos >= d.pending.size();
}

bool Decoder::can_seek() const
{
    const Impl& d = *impl_;
    if (!d.open_ || d.format_ctx == nullptr) {
        return false;
    }

    // `pb` is null for a format that reads through its own means rather than an
    // AVIOContext. Nothing this project opens does that, but reading `seekable`
    // off a null pointer would be a crash on the one source that did.
    if (d.format_ctx->pb == nullptr) {
        return false;
    }

    // AVIO_SEEKABLE_NORMAL is the byte-seekable flag, and it is what tells a
    // local file apart from a pipe -- and, for the case that matters here, an
    // HTTP source whose server honours range requests from one that does not.
    // FFmpeg works that out during avformat_open_input, so this costs nothing.
    return (d.format_ctx->pb->seekable & AVIO_SEEKABLE_NORMAL) != 0;
}

DecoderError Decoder::seek(double position_seconds)
{
    Impl& d = *impl_;
    if (!d.open_) {
        return DecoderError::kNotOpen;
    }
    if (position_seconds < 0.0) {
        position_seconds = 0.0;
    }

    // Into the STREAM's own time base, not AV_TIME_BASE.
    //
    // av_seek_frame with a stream index interprets the timestamp in that
    // stream's units, and an mp3 at 44.1 kHz does not use microseconds. Passing
    // an AV_TIME_BASE value with a stream index lands somewhere arbitrary --
    // usually near the start, which reads as "seeking always jumps to the
    // beginning" rather than as a units bug.
    const AVStream* stream = d.format_ctx->streams[static_cast<unsigned>(d.stream_index)];
    const auto      target = static_cast<std::int64_t>(
        position_seconds / (static_cast<double>(stream->time_base.num) /
                            static_cast<double>(stream->time_base.den)));

    // AVSEEK_FLAG_BACKWARD: land at or before the request. Without it the seek
    // goes to the next keyframe AFTER the target, so every seek silently skips
    // a little of the audio the listener asked to hear -- and on a
    // sparsely-indexed file, quite a lot of it.
    if (av_seek_frame(d.format_ctx, d.stream_index, target, AVSEEK_FLAG_BACKWARD) < 0) {
        return DecoderError::kBackendFailure;
    }

    // THE DECODER STILL HOLDS FRAMES FROM BEFORE THE SEEK. Without this flush
    // the first audio after a seek is the tail of wherever playback used to be,
    // which sounds like a brief stutter of the previous position and is very
    // hard to attribute to seeking.
    avcodec_flush_buffers(d.codec_ctx);

    d.pending.clear();
    d.pending_pos = 0;
    d.eof         = false;
    d.drained     = false;

    return DecoderError::kOk;
}

std::size_t Decoder::read(float* out, std::size_t max_frames)
{
    Impl& d = *impl_;
    if (!d.open_ || out == nullptr || max_frames == 0) {
        return 0;
    }

    const std::size_t channels = std::size_t(d.info.channels);
    std::size_t       written  = 0;

    while (written < max_frames) {
        if (d.pending_pos >= d.pending.size()) {
            d.pending.clear();
            d.pending_pos = 0;
            if (!d.decode_more()) {
                break;
            }
            continue;
        }

        const std::size_t available = (d.pending.size() - d.pending_pos) / channels;
        const std::size_t take      = std::min(available, max_frames - written);
        if (take == 0) {
            break;
        }

        std::memcpy(out + written * channels,
                    d.pending.data() + d.pending_pos,
                    take * channels * sizeof(float));

        d.pending_pos += take * channels;
        written += take;
    }

    return written;
}

// ===========================================================================
// Resampler
// ===========================================================================

struct Resampler::Impl {
    ~Impl()
    {
        if (swr != nullptr) {
            swr_free(&swr);
        }
    }

    SwrContext*   swr             = nullptr;
    std::uint32_t source_rate     = 0;
    std::uint16_t source_channels = 0;
};

Resampler::Resampler() : impl_(std::make_unique<Impl>()) {}
Resampler::~Resampler() = default;

DecoderError Resampler::configure(std::uint32_t source_rate, std::uint16_t source_channels)
{
    Impl& r = *impl_;

    if (source_rate == 0 || source_channels == 0) {
        return DecoderError::kBackendFailure;
    }

    if (r.swr != nullptr) {
        swr_free(&r.swr);
    }

    AVChannelLayout in_layout{};
    AVChannelLayout out_layout{};
    av_channel_layout_default(&in_layout, int(source_channels));
    av_channel_layout_default(&out_layout, 2);  // the tap is always stereo

    const int ret = swr_alloc_set_opts2(&r.swr,
                                        &out_layout, AV_SAMPLE_FMT_FLT, int(kAnalysisRate),
                                        &in_layout, AV_SAMPLE_FMT_FLT, int(source_rate),
                                        0, nullptr);

    av_channel_layout_uninit(&in_layout);
    av_channel_layout_uninit(&out_layout);

    if (ret < 0 || swr_init(r.swr) < 0) {
        if (r.swr != nullptr) {
            swr_free(&r.swr);
        }
        return DecoderError::kBackendFailure;
    }

    r.source_rate     = source_rate;
    r.source_channels = source_channels;
    return DecoderError::kOk;
}

bool Resampler::is_configured() const { return impl_->swr != nullptr; }

std::size_t Resampler::max_output_frames(std::size_t input_frames) const
{
    const Impl& r = *impl_;
    if (r.swr == nullptr) {
        return 0;
    }
    // Delay-aware worst case, plus slack for rounding.
    const int64_t delay = swr_get_delay(r.swr, int64_t(r.source_rate));
    const int64_t out   = av_rescale_rnd(delay + int64_t(input_frames),
                                         int64_t(kAnalysisRate), int64_t(r.source_rate),
                                         AV_ROUND_UP);
    return std::size_t(out) + 32;
}

std::size_t Resampler::process(const float* in, std::size_t input_frames,
                               float* out, std::size_t max_output)
{
    Impl& r = *impl_;
    if (r.swr == nullptr || out == nullptr || max_output == 0) {
        return 0;
    }

    const auto*    src     = reinterpret_cast<const std::uint8_t*>(in);
    auto*          dst     = reinterpret_cast<std::uint8_t*>(out);
    const int      produced = swr_convert(r.swr, &dst, int(max_output),
                                          (in != nullptr && input_frames > 0) ? &src : nullptr,
                                          int(input_frames));
    return (produced > 0) ? std::size_t(produced) : 0;
}

std::size_t Resampler::flush(float* out, std::size_t max_output)
{
    return process(nullptr, 0, out, max_output);
}

}  // namespace holocron
