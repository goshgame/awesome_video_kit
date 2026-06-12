//
//  ffmpeg_mp4_transcoder.cpp
//  AwesomeVideoKitSDK
//
//  Created by dev on 2026/3/20.
//

#include "ffmpeg_mp4_transcoder.h"
#include "ffmpeg_common_utils.h"

#include <chrono>
#include <thread>

namespace {

bool packetLooksLikeAnnexBForInputDebug(const AVPacket *packet) {
    if (!packet || !packet->data || packet->size < 4) return false;
    return (packet->data[0] == 0 && packet->data[1] == 0 && packet->data[2] == 1) ||
        (packet->size >= 5 &&
         packet->data[0] == 0 &&
         packet->data[1] == 0 &&
         packet->data[2] == 0 &&
         packet->data[3] == 1);
}

std::string packetHexPreviewForInputDebug(const AVPacket *packet, size_t max_bytes = 16) {
    if (!packet || !packet->data || packet->size <= 0) return "empty";

    std::string preview;
    const size_t limit = static_cast<size_t>(packet->size) < max_bytes ? static_cast<size_t>(packet->size) : max_bytes;
    preview.reserve(limit * 2);
    char buffer[4];
    for (size_t index = 0; index < limit; ++index) {
        snprintf(buffer, sizeof(buffer), "%02X", packet->data[index]);
        preview += buffer;
    }
    return preview;
}

std::string extradataPreviewForInputDebug(const AVCodecParameters *codecpar, size_t max_bytes = 16) {
    if (!codecpar || !codecpar->extradata || codecpar->extradata_size <= 0) return "empty";

    std::string preview;
    const size_t size = static_cast<size_t>(codecpar->extradata_size);
    const size_t limit = size < max_bytes ? size : max_bytes;
    preview.reserve(limit * 2);
    char buffer[4];
    for (size_t index = 0; index < limit; ++index) {
        snprintf(buffer, sizeof(buffer), "%02X", codecpar->extradata[index]);
        preview += buffer;
    }
    return preview;
}

std::string codecTagForInputDebug(uint32_t codec_tag) {
    if (codec_tag == 0) return "0";

    char buffer[32];
    const uint8_t a = static_cast<uint8_t>(codec_tag & 0xFF);
    const uint8_t b = static_cast<uint8_t>((codec_tag >> 8) & 0xFF);
    const uint8_t c = static_cast<uint8_t>((codec_tag >> 16) & 0xFF);
    const uint8_t d = static_cast<uint8_t>((codec_tag >> 24) & 0xFF);
    snprintf(
        buffer,
        sizeof(buffer),
        "%c%c%c%c/0x%08X",
        a >= 32 && a <= 126 ? a : '.',
        b >= 32 && b <= 126 ? b : '.',
        c >= 32 && c <= 126 ? c : '.',
        d >= 32 && d <= 126 ? d : '.',
        codec_tag
    );
    return std::string(buffer);
}

} // namespace

FFmpegMp4Transcoder::FFmpegMp4Transcoder(
    const char *input_path,
    const char *output_path,
    const FFmpegMp4TranscodeConfig *config,
    void (*progress_cb)(int percentage, void *user_data),
    void *user_data,
    volatile int *cancel_flag,
    volatile int *pause_flag
) : FFmpegMp4TranscoderBase(
        input_path,
        output_path,
        config,
        progress_cb,
        user_data,
        cancel_flag,
        pause_flag
    ),
    video_enc_frame_(nullptr),
    last_video_frame_(nullptr),
    next_video_pts_(0),
    input_video_debug_packets_logged_(0) {
}

FFmpegMp4Transcoder::~FFmpegMp4Transcoder() {
    cleanup();
}

void FFmpegMp4Transcoder::cleanup() {
    if (last_video_frame_) av_frame_free(&last_video_frame_);
    if (video_enc_frame_) av_frame_free(&video_enc_frame_);
    cleanupSharedResources();
    next_video_pts_ = 0;
    input_video_debug_packets_logged_ = 0;
}

int FFmpegMp4Transcoder::openInput() {
    const int ret = openInputFileAndInitDecoders();
    if (ret < 0) return ret;

    if (video_input_index_ >= 0 && input_format_ && input_format_->streams[video_input_index_]) {
        AVStream *video_stream = input_format_->streams[video_input_index_];
        const AVCodecParameters *codecpar = video_stream->codecpar;
        AWESOME_FF_LOGI(
            "Input video stream: codec=%s codec_tag=%s size=%dx%d time_base=%d/%d avg_frame_rate=%d/%d extradata_size=%d extradata_head=%s",
            codecpar ? avcodec_get_name(codecpar->codec_id) : "unknown",
            codecpar ? codecTagForInputDebug(codecpar->codec_tag).c_str() : "unknown",
            codecpar ? codecpar->width : 0,
            codecpar ? codecpar->height : 0,
            video_stream->time_base.num,
            video_stream->time_base.den,
            video_stream->avg_frame_rate.num,
            video_stream->avg_frame_rate.den,
            codecpar ? codecpar->extradata_size : 0,
            extradataPreviewForInputDebug(codecpar).c_str()
        );
    }

    progress_.reset(FFmpegCommonUtils::getDurationInUsSafe(input_format_), progress_cb_, user_data_);
    return 0;
}

int FFmpegMp4Transcoder::openOutput() {
    int ret = avformat_alloc_output_context2(&output_format_, nullptr, "mp4", output_path_.c_str());
    if (ret < 0) return ret;

    ret = initializeVideoEncoder();
    if (ret < 0) {
        FFmpegCommonUtils::printError("Failed to initialize video encoder", ret);
        return ret;
    }

    if (audio_dec_ctx_) {
        ret = initializeAudioEncoder();
        if (ret == AVERROR_ENCODER_NOT_FOUND &&
            input_format_->streams[audio_input_index_]->codecpar->codec_id == AV_CODEC_ID_AAC) {
            AWESOME_FF_LOGW("AAC encoder unavailable. Falling back to copying the input AAC stream.");
            ret = initializeAudioCopyStream();
        }
        if (ret < 0) {
            FFmpegCommonUtils::printError("Failed to initialize audio encoder", ret);
            return ret;
        }
    }

    ret = initializeFrames();
    if (ret < 0) return ret;

    if (!(output_format_->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&output_format_->pb, output_path_.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) return ret;
    }

    av_dict_copy(&output_format_->metadata, input_format_->metadata, 0);
    return 0;
}

int FFmpegMp4Transcoder::initializeVideoEncoder() {
    return configureVideoEncoder(true);
}

int FFmpegMp4Transcoder::configureVideoEncoder(bool create_output_stream) {
    AVStream *input_stream = input_format_->streams[video_input_index_];
    AVStream *output_stream = nullptr;
    int ret = 0;
    int output_width = 0;
    int output_height = 0;

    const AVCodec *encoder = (!create_output_stream && video_enc_ctx_ && video_enc_ctx_->codec)
        ? video_enc_ctx_->codec
        : FFmpegCommonUtils::findPreferredVideoEncoder(AV_CODEC_ID_H264);
    if (!encoder) return AVERROR_ENCODER_NOT_FOUND;

    const AVRational effective_frame_rate = resolveVideoFrameRate(input_format_, input_stream);

    ret = FFmpegCommonUtils::resolveOutputVideoSize(
        video_dec_ctx_->width,
        video_dec_ctx_->height,
        scale_width_,
        scale_height_,
        &output_width,
        &output_height
    );
    if (ret < 0) return ret;

    auto configureEncoderContext = [&](const AVCodec *candidate) -> int {
        if (video_enc_ctx_) avcodec_free_context(&video_enc_ctx_);

        video_enc_ctx_ = avcodec_alloc_context3(candidate);
        if (!video_enc_ctx_) return AVERROR(ENOMEM);

        video_enc_ctx_->codec_id = candidate->id;
        video_enc_ctx_->codec_type = AVMEDIA_TYPE_VIDEO;
        video_enc_ctx_->width = output_width;
        video_enc_ctx_->height = output_height;
        video_enc_ctx_->sample_aspect_ratio = video_dec_ctx_->sample_aspect_ratio;
        if (video_enc_ctx_->sample_aspect_ratio.num <= 0 || video_enc_ctx_->sample_aspect_ratio.den <= 0) {
            video_enc_ctx_->sample_aspect_ratio = input_stream->sample_aspect_ratio;
        }
        if (video_enc_ctx_->sample_aspect_ratio.num <= 0 || video_enc_ctx_->sample_aspect_ratio.den <= 0) {
            video_enc_ctx_->sample_aspect_ratio = AVRational{1, 1};
        }
        video_enc_ctx_->pix_fmt = FFmpegCommonUtils::chooseVideoPixelFormat(candidate);
        video_enc_ctx_->time_base = av_inv_q(effective_frame_rate);
        if (video_enc_ctx_->time_base.num <= 0 || video_enc_ctx_->time_base.den <= 0) {
            video_enc_ctx_->time_base = input_stream->time_base;
        }
        if (video_enc_ctx_->time_base.num <= 0 || video_enc_ctx_->time_base.den <= 0) {
            video_enc_ctx_->time_base = AVRational{1, 25};
        }
        video_enc_ctx_->framerate = effective_frame_rate;
        video_enc_ctx_->bit_rate = input_stream->codecpar->bit_rate > 0
            ? input_stream->codecpar->bit_rate
            : 2 * 1000 * 1000;
        video_enc_ctx_->gop_size = videoGopSize(effective_frame_rate);
        video_enc_ctx_->max_b_frames = 0;
        video_enc_ctx_->color_range = video_dec_ctx_->color_range;
        video_enc_ctx_->color_primaries = video_dec_ctx_->color_primaries;
        video_enc_ctx_->color_trc = video_dec_ctx_->color_trc;
        video_enc_ctx_->colorspace = video_dec_ctx_->colorspace;
        video_enc_ctx_->profile = FF_PROFILE_H264_HIGH;
        {
            const int parsed_level = FFmpegCommonUtils::parseH264Level(video_level_);
            if (parsed_level > 0) video_enc_ctx_->level = parsed_level;
        }

        if (candidate->capabilities & AV_CODEC_CAP_EXPERIMENTAL) {
            video_enc_ctx_->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
        }

        if ((output_format_->oformat->flags & AVFMT_GLOBALHEADER) &&
            !FFmpegCommonUtils::isMediaCodecEncoder(candidate)) {
            video_enc_ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }

        FFmpegCommonUtils::sanitizeVideoEncoderContextDefaults(video_enc_ctx_, candidate);
        return 0;
    };

    auto openEncoder = [&](const AVCodec *candidate) -> int {
        AVDictionary *codec_options = nullptr;
        char crf_buffer[16];

        av_dict_set(&codec_options, "profile", video_profile_.c_str(), 0);
        av_dict_set(&codec_options, "level", video_level_.c_str(), 0);
        if (FFmpegCommonUtils::isVideoToolboxEncoder(candidate)) {
            // 540p 模拟 CRF23（标准）
            av_dict_set(&codec_options, "b:v", "800k", 0);
            av_dict_set(&codec_options, "maxrate", "1600k", 0);
            av_dict_set(&codec_options, "bufsize", "1600k", 0);

            // 低延迟
            av_dict_set(&codec_options, "realtime", "true", 0);
            av_dict_set(&codec_options, "forced-idr", "true", 0);
            if (!create_output_stream && video_packets_written_) {
                av_dict_set(&codec_options, "frames_before", "true", 0);
            }
        } else if (FFmpegCommonUtils::isMediaCodecEncoder(candidate)) {
            // 540p 模拟 CRF23（标准）
            av_dict_set(&codec_options, "b:v", "800k", 0);
            av_dict_set(&codec_options, "maxrate", "1600k", 0);
            av_dict_set(&codec_options, "bufsize", "1600k", 0);
            if (output_format_->oformat->flags & AVFMT_GLOBALHEADER) {
                av_dict_set(&codec_options, "ndk_async", "false", 0);
            }
        } else {
            snprintf(crf_buffer, sizeof(crf_buffer), "%d", video_crf_);
            av_dict_set(&codec_options, "preset", video_preset_.c_str(), 0);
            av_dict_set(&codec_options, "crf", crf_buffer, 0);
        }

        const int open_ret = avcodec_open2(video_enc_ctx_, candidate, &codec_options);
        av_dict_free(&codec_options);
        return open_ret;
    };

    ret = configureEncoderContext(encoder);
    if (ret < 0) return ret;

    if (scale_width_ > 0 || scale_height_ > 0) {
        AWESOME_FF_LOGI(
            "Requested output video size: %dx%d, actual encoder size: %dx%d",
            scale_width_ > 0 ? scale_width_ : 0,
            scale_height_ > 0 ? scale_height_ : 0,
            video_enc_ctx_->width,
            video_enc_ctx_->height
        );
    }

    if (create_output_stream) {
        ret = FFmpegCommonUtils::reopenVideoEncoderWithFallback(
            "video encoder",
            &encoder,
            FFmpegCommonUtils::findSoftwareVideoEncoder(AV_CODEC_ID_H264),
            openEncoder(encoder),
            configureEncoderContext,
            openEncoder
        );
        if (ret < 0) {
            AWESOME_FF_LOGE("Failed to open video encoder: %s", FFmpegCommonUtils::codecNameOrUnknown(encoder));
            return ret;
        }
    } else {
        ret = openEncoder(encoder);
        if (ret < 0) {
            AWESOME_FF_LOGE(
                "Failed to reopen runtime video encoder without software fallback: %s",
                FFmpegCommonUtils::codecNameOrUnknown(encoder)
            );
            return ret;
        }
    }

    if (create_output_stream) {
        output_stream = avformat_new_stream(output_format_, nullptr);
        if (!output_stream) return AVERROR(ENOMEM);
        video_output_index_ = output_stream->index;
        output_stream->disposition = input_stream->disposition;
        av_dict_copy(&output_stream->metadata, input_stream->metadata, 0);
    }

    ret = updateVideoOutputStreamAfterEncoderChange();
    if (ret < 0) return ret;

    ret = FFmpegCommonUtils::copyDisplayMatrixSideData(
        input_stream,
        output_format_->streams[video_output_index_]
    );
    if (ret < 0) return ret;

    return 0;
}

int FFmpegMp4Transcoder::initializeAudioEncoder() {
    AVStream *input_stream = input_format_->streams[audio_input_index_];
    AVStream *output_stream = nullptr;
    AVChannelLayout selected_layout{};
    int ret = 0;

    const AVCodec *encoder = FFmpegCommonUtils::findPreferredAudioEncoder(AV_CODEC_ID_AAC);
    if (!encoder) {
        FFmpegCommonUtils::printMissingAacEncoderHint();
        return AVERROR_ENCODER_NOT_FOUND;
    }
    AWESOME_FF_LOGI("Using audio encoder: %s", FFmpegCommonUtils::codecNameOrUnknown(encoder));

    audio_enc_ctx_ = avcodec_alloc_context3(encoder);
    if (!audio_enc_ctx_) return AVERROR(ENOMEM);

    audio_enc_ctx_->codec_id = encoder->id;
    audio_enc_ctx_->codec_type = AVMEDIA_TYPE_AUDIO;
    audio_enc_ctx_->sample_rate = FFmpegCommonUtils::chooseSampleRate(encoder, audio_dec_ctx_->sample_rate);
    audio_enc_ctx_->sample_fmt = FFmpegCommonUtils::chooseSampleFormat(encoder, audio_dec_ctx_->sample_fmt);
    audio_enc_ctx_->bit_rate = audio_bitrate_;
    audio_enc_ctx_->time_base = AVRational{1, audio_enc_ctx_->sample_rate};

    ret = FFmpegCommonUtils::chooseChannelLayout(encoder, &audio_dec_ctx_->ch_layout, &selected_layout);
    if (ret < 0) goto end;
    ret = av_channel_layout_copy(&audio_enc_ctx_->ch_layout, &selected_layout);
    if (ret < 0) goto end;

    if (encoder->capabilities & AV_CODEC_CAP_EXPERIMENTAL) {
        audio_enc_ctx_->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
    }

    if (output_format_->oformat->flags & AVFMT_GLOBALHEADER) {
        audio_enc_ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    ret = avcodec_open2(audio_enc_ctx_, encoder, nullptr);
    if (ret < 0) {
        AWESOME_FF_LOGE("Failed to open audio encoder: %s", FFmpegCommonUtils::codecNameOrUnknown(encoder));
        goto end;
    }

    output_stream = avformat_new_stream(output_format_, nullptr);
    if (!output_stream) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    audio_output_index_ = output_stream->index;

    ret = avcodec_parameters_from_context(output_stream->codecpar, audio_enc_ctx_);
    if (ret < 0) goto end;

    output_stream->codecpar->codec_tag = 0;
    output_stream->time_base = audio_enc_ctx_->time_base;
    output_stream->disposition = input_stream->disposition;
    av_dict_copy(&output_stream->metadata, input_stream->metadata, 0);

    ret = FFmpegCommonUtils::copyDisplayMatrixSideData(input_stream, output_stream);
    if (ret < 0) goto end;

    ret = swr_alloc_set_opts2(
        &swr_ctx_,
        &audio_enc_ctx_->ch_layout,
        audio_enc_ctx_->sample_fmt,
        audio_enc_ctx_->sample_rate,
        &audio_dec_ctx_->ch_layout,
        audio_dec_ctx_->sample_fmt,
        audio_dec_ctx_->sample_rate,
        0,
        nullptr
    );
    if (ret < 0) goto end;

    ret = swr_init(swr_ctx_);
    if (ret < 0) goto end;

    audio_fifo_ = av_audio_fifo_alloc(
        audio_enc_ctx_->sample_fmt,
        audio_enc_ctx_->ch_layout.nb_channels,
        1
    );
    if (!audio_fifo_) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

end:
    av_channel_layout_uninit(&selected_layout);
    return ret;
}

int FFmpegMp4Transcoder::initializeAudioCopyStream() {
    AVStream *output_stream = avformat_new_stream(output_format_, nullptr);
    if (!output_stream) return AVERROR(ENOMEM);

    const int ret = initializeAudioCopyStreamWithOptionalBsf(output_stream);
    if (ret < 0) return ret;

    return 0;
}

int FFmpegMp4Transcoder::initializeFrames() {
    video_dec_frame_ = av_frame_alloc();
    video_enc_frame_ = av_frame_alloc();
    enc_packet_ = av_packet_alloc();
    if (!video_dec_frame_ || !video_enc_frame_ || !enc_packet_) {
        return AVERROR(ENOMEM);
    }

    video_enc_frame_->format = video_enc_ctx_->pix_fmt;
    video_enc_frame_->width = video_enc_ctx_->width;
    video_enc_frame_->height = video_enc_ctx_->height;
    video_enc_frame_->sample_aspect_ratio = video_enc_ctx_->sample_aspect_ratio;

    int ret = av_frame_get_buffer(video_enc_frame_, 32);
    if (ret < 0) return ret;

    if (audio_dec_ctx_) {
        audio_dec_frame_ = av_frame_alloc();
        if (!audio_dec_frame_) return AVERROR(ENOMEM);
    }
    return 0;
}

int FFmpegMp4Transcoder::refreshVideoEncodeBuffer() {
    if (!video_enc_ctx_) return AVERROR(EINVAL);
    if (video_enc_frame_) av_frame_free(&video_enc_frame_);

    video_enc_frame_ = av_frame_alloc();
    if (!video_enc_frame_) return AVERROR(ENOMEM);

    video_enc_frame_->format = video_enc_ctx_->pix_fmt;
    video_enc_frame_->width = video_enc_ctx_->width;
    video_enc_frame_->height = video_enc_ctx_->height;
    video_enc_frame_->sample_aspect_ratio = video_enc_ctx_->sample_aspect_ratio;

    return av_frame_get_buffer(video_enc_frame_, 32);
}

int FFmpegMp4Transcoder::rebuildVideoEncoderForResume() {
    const int ret = configureVideoEncoder(false);
    if (ret < 0) return ret;

    if (last_video_frame_) av_frame_free(&last_video_frame_);
    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }

    return refreshVideoEncodeBuffer();
}

int FFmpegMp4Transcoder::sendPreparedVideoFrame(AVFrame *frame, int64_t output_pts) {
    if (!frame) return AVERROR(EINVAL);

    frame->pict_type = AV_PICTURE_TYPE_NONE;
    int ret = sendVideoFrameWithRecovery(frame, output_pts, "Transcoder video frame encode");
    if (ret < 0) return ret;

    ret = cacheFrameReference(&last_video_frame_, frame);
    if (ret < 0) return ret;

    if (output_pts != AV_NOPTS_VALUE && output_pts + 1 > next_video_pts_) {
        next_video_pts_ = output_pts + 1;
    }
    return 0;
}

int FFmpegMp4Transcoder::duplicateLastVideoFrameUntil(int64_t target_pts) {
    if (!hasForcedVideoFrameRate() || target_pts == AV_NOPTS_VALUE || !last_video_frame_) {
        return 0;
    }

    while (next_video_pts_ < target_pts) {
        int ret = waitIfPaused();
        if (ret < 0) return ret;
        ret = rebuildVideoEncoderAfterPauseIfNeeded("Rebuilding hardware video encoder after pause/resume.");
        if (ret < 0) return ret;
        if (!last_video_frame_) return 0;

        AVFrame *duplicate_frame = av_frame_clone(last_video_frame_);
        if (!duplicate_frame) return AVERROR(ENOMEM);

        ret = sendPreparedVideoFrame(duplicate_frame, next_video_pts_);
        av_frame_free(&duplicate_frame);
        if (ret < 0) return ret;
    }

    return 0;
}

int FFmpegMp4Transcoder::processDecodedVideoFrame(AVFrame *frame) {
    if (!frame) return AVERROR(EINVAL);

    int ret = waitIfPaused();
    if (ret < 0) return ret;
    ret = rebuildVideoEncoderAfterPauseIfNeeded("Rebuilding hardware video encoder after pause/resume.");
    if (ret < 0) return ret;

    sws_ctx_ = sws_getCachedContext(
        sws_ctx_,
        frame->width,
        frame->height,
        static_cast<AVPixelFormat>(frame->format),
        video_enc_ctx_->width,
        video_enc_ctx_->height,
        video_enc_ctx_->pix_fmt,
        SWS_BICUBIC,
        nullptr,
        nullptr,
        nullptr
    );
    if (!sws_ctx_) return AVERROR(ENOMEM);

    ret = av_frame_make_writable(video_enc_frame_);
    if (ret < 0) return ret;

    const int scaled = sws_scale(
        sws_ctx_,
        frame->data,
        frame->linesize,
        0,
        frame->height,
        video_enc_frame_->data,
        video_enc_frame_->linesize
    );
    if (scaled <= 0) return AVERROR_EXTERNAL;

    const int64_t source_pts = frame->pts != AV_NOPTS_VALUE ? frame->pts : frame->best_effort_timestamp;
    const int64_t normalized_pts = normalizePts(
        source_pts,
        input_format_->streams[video_input_index_]->time_base,
        video_enc_ctx_->time_base
    );
    video_enc_frame_->pict_type = AV_PICTURE_TYPE_NONE;
    video_enc_frame_->sample_aspect_ratio = frame->sample_aspect_ratio.num > 0 && frame->sample_aspect_ratio.den > 0
        ? frame->sample_aspect_ratio
        : video_enc_ctx_->sample_aspect_ratio;
    video_enc_frame_->color_range = frame->color_range;
    video_enc_frame_->color_primaries = frame->color_primaries;
    video_enc_frame_->color_trc = frame->color_trc;
    video_enc_frame_->colorspace = frame->colorspace;

    if (source_pts != AV_NOPTS_VALUE) {
        progress_.update(source_pts, input_format_->streams[video_input_index_]->time_base);
    }

    if (!hasForcedVideoFrameRate()) {
        return sendPreparedVideoFrame(video_enc_frame_, normalized_pts);
    }

    int64_t output_pts = normalized_pts;
    if (output_pts == AV_NOPTS_VALUE) {
        output_pts = next_video_pts_;
    }
    if (!last_video_frame_ && output_pts > next_video_pts_) {
        output_pts = next_video_pts_;
    }

    ret = duplicateLastVideoFrameUntil(output_pts);
    if (ret < 0) return ret;

    if (output_pts < next_video_pts_) {
        return 0;
    }

    return sendPreparedVideoFrame(video_enc_frame_, output_pts);
}

int FFmpegMp4Transcoder::processVideoPacket(AVPacket *packet) {
    int ret = normalizeInputVideoPacketForDecoder(packet);
    if (ret < 0) return ret;

    if (packet &&
        input_video_debug_packets_logged_ < 3 &&
        input_format_ &&
        video_input_index_ >= 0 &&
        input_format_->streams[video_input_index_]) {
        const AVCodecParameters *codecpar = input_format_->streams[video_input_index_]->codecpar;
        AWESOME_FF_LOGI(
            "Input video packet[%d]: size=%d pts=%lld dts=%lld annexb=%d head=%s stream_extradata_size=%d stream_extradata_head=%s",
            input_video_debug_packets_logged_,
            packet->size,
            static_cast<long long>(packet->pts),
            static_cast<long long>(packet->dts),
            packetLooksLikeAnnexBForInputDebug(packet) ? 1 : 0,
            packetHexPreviewForInputDebug(packet).c_str(),
            codecpar ? codecpar->extradata_size : 0,
            extradataPreviewForInputDebug(codecpar).c_str()
        );
        input_video_debug_packets_logged_ += 1;
    }

    ret = avcodec_send_packet(video_dec_ctx_, packet);
    if (ret < 0) return ret;

    while ((ret = avcodec_receive_frame(video_dec_ctx_, video_dec_frame_)) >= 0) {
        if (video_dec_frame_->pts == AV_NOPTS_VALUE) {
            video_dec_frame_->pts = video_dec_frame_->best_effort_timestamp;
        }

        ret = processDecodedVideoFrame(video_dec_frame_);
        av_frame_unref(video_dec_frame_);
        if (ret < 0) return ret;
    }

    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return 0;
    return ret;
}

int FFmpegMp4Transcoder::processDecodedAudioFrame(AVFrame *frame) {
    if (!frame || !audio_enc_ctx_ || !swr_ctx_ || !audio_fifo_) return AVERROR(EINVAL);

    const int64_t dst_nb_samples_value = av_rescale_rnd(
        swr_get_delay(swr_ctx_, audio_dec_ctx_->sample_rate) + frame->nb_samples,
        audio_enc_ctx_->sample_rate,
        audio_dec_ctx_->sample_rate,
        AV_ROUND_UP
    );
    if (dst_nb_samples_value > INT_MAX) return AVERROR(EINVAL);
    const int dst_nb_samples = static_cast<int>(dst_nb_samples_value);
    if (dst_nb_samples <= 0) return 0;

    AVFrame *converted_frame = av_frame_alloc();
    if (!converted_frame) return AVERROR(ENOMEM);

    converted_frame->nb_samples = dst_nb_samples;
    converted_frame->format = audio_enc_ctx_->sample_fmt;
    converted_frame->sample_rate = audio_enc_ctx_->sample_rate;

    int ret = av_channel_layout_copy(&converted_frame->ch_layout, &audio_enc_ctx_->ch_layout);
    if (ret < 0) goto end;

    ret = av_frame_get_buffer(converted_frame, 0);
    if (ret < 0) goto end;

    ret = swr_convert(
        swr_ctx_,
        converted_frame->data,
        dst_nb_samples,
        const_cast<const uint8_t **>(frame->extended_data),
        frame->nb_samples
    );
    if (ret < 0) goto end;

    converted_frame->nb_samples = ret;
    if (converted_frame->nb_samples > 0) {
        ret = av_audio_fifo_realloc(
            audio_fifo_,
            av_audio_fifo_size(audio_fifo_) + converted_frame->nb_samples
        );
        if (ret < 0) goto end;

        ret = av_audio_fifo_write(
            audio_fifo_,
            reinterpret_cast<void **>(converted_frame->data),
            converted_frame->nb_samples
        );
        if (ret < converted_frame->nb_samples) {
            ret = ret < 0 ? ret : AVERROR(EIO);
            goto end;
        }
    }

    if (audio_next_pts_ == AV_NOPTS_VALUE) {
        const int64_t source_pts = frame->pts != AV_NOPTS_VALUE ? frame->pts : frame->best_effort_timestamp;
        audio_next_pts_ = normalizePts(
            source_pts,
            input_format_->streams[audio_input_index_]->time_base,
            audio_enc_ctx_->time_base
        );
        if (audio_next_pts_ == AV_NOPTS_VALUE) audio_next_pts_ = 0;
    }

    ret = drainAudioFifo(0);

end:
    if (converted_frame) av_frame_free(&converted_frame);
    return ret;
}

int FFmpegMp4Transcoder::processAudioPacket(AVPacket *packet) {
    int ret = waitIfPaused();
    if (ret < 0) return ret;

    ret = avcodec_send_packet(audio_dec_ctx_, packet);
    if (ret < 0) return ret;

    while ((ret = avcodec_receive_frame(audio_dec_ctx_, audio_dec_frame_)) >= 0) {
        if (audio_dec_frame_->pts == AV_NOPTS_VALUE) {
            audio_dec_frame_->pts = audio_dec_frame_->best_effort_timestamp;
        }

        ret = processDecodedAudioFrame(audio_dec_frame_);
        av_frame_unref(audio_dec_frame_);
        if (ret < 0) return ret;
    }

    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return 0;
    return ret;
}

int FFmpegMp4Transcoder::processAudioPacketCopy(AVPacket *packet) {
    return processAudioCopyPacketWithOptionalBsf(packet);
}

int FFmpegMp4Transcoder::drainAudioFifo(int flush_last_frame) {
    if (!audio_enc_ctx_ || !audio_fifo_) return 0;

    const int fixed_frame_size = audio_enc_ctx_->frame_size;
    const int variable_frame_size = audio_enc_ctx_->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE;

    while (av_audio_fifo_size(audio_fifo_) > 0) {
        const int pause_ret = waitIfPaused();
        if (pause_ret < 0) return pause_ret;

        int target_samples = 0;
        if (!fixed_frame_size || variable_frame_size) {
            target_samples = av_audio_fifo_size(audio_fifo_);
        } else {
            if (!flush_last_frame && av_audio_fifo_size(audio_fifo_) < fixed_frame_size) {
                break;
            }
            target_samples = fixed_frame_size;
        }

        AVFrame *frame = av_frame_alloc();
        if (!frame) return AVERROR(ENOMEM);

        frame->nb_samples = target_samples;
        frame->format = audio_enc_ctx_->sample_fmt;
        frame->sample_rate = audio_enc_ctx_->sample_rate;

        int ret = av_channel_layout_copy(&frame->ch_layout, &audio_enc_ctx_->ch_layout);
        if (ret < 0) {
            av_frame_free(&frame);
            return ret;
        }

        ret = av_frame_get_buffer(frame, 0);
        if (ret < 0) {
            av_frame_free(&frame);
            return ret;
        }

        const int available = av_audio_fifo_size(audio_fifo_);
        const int read_samples = available < target_samples ? available : target_samples;
        ret = av_audio_fifo_read(audio_fifo_, reinterpret_cast<void **>(frame->data), read_samples);
        if (ret < read_samples) {
            av_frame_free(&frame);
            return ret < 0 ? ret : AVERROR(EIO);
        }

        if (read_samples < target_samples) {
            av_samples_set_silence(
                frame->data,
                read_samples,
                target_samples - read_samples,
                audio_enc_ctx_->ch_layout.nb_channels,
                audio_enc_ctx_->sample_fmt
            );
        }

        if (audio_next_pts_ == AV_NOPTS_VALUE) audio_next_pts_ = 0;
        frame->pts = audio_next_pts_;
        audio_next_pts_ += frame->nb_samples;

        ret = avcodec_send_frame(audio_enc_ctx_, frame);
        av_frame_free(&frame);
        if (ret < 0) return ret;

        ret = writeEncodedPackets(audio_enc_ctx_, audio_output_index_);
        if (ret < 0) return ret;
    }

    return 0;
}

int FFmpegMp4Transcoder::flushResamplerToFifo() {
    if (!swr_ctx_ || !audio_enc_ctx_ || !audio_fifo_) return 0;

    while (swr_get_delay(swr_ctx_, audio_dec_ctx_->sample_rate) > 0) {
        const int pause_ret = waitIfPaused();
        if (pause_ret < 0) return pause_ret;

        const int64_t dst_nb_samples_value = av_rescale_rnd(
            swr_get_delay(swr_ctx_, audio_dec_ctx_->sample_rate),
            audio_enc_ctx_->sample_rate,
            audio_dec_ctx_->sample_rate,
            AV_ROUND_UP
        );
        if (dst_nb_samples_value > INT_MAX) return AVERROR(EINVAL);
        const int dst_nb_samples = static_cast<int>(dst_nb_samples_value);
        if (dst_nb_samples <= 0) break;

        AVFrame *flush_frame = av_frame_alloc();
        if (!flush_frame) return AVERROR(ENOMEM);

        flush_frame->nb_samples = dst_nb_samples;
        flush_frame->format = audio_enc_ctx_->sample_fmt;
        flush_frame->sample_rate = audio_enc_ctx_->sample_rate;

        int ret = av_channel_layout_copy(&flush_frame->ch_layout, &audio_enc_ctx_->ch_layout);
        if (ret < 0) {
            av_frame_free(&flush_frame);
            return ret;
        }

        ret = av_frame_get_buffer(flush_frame, 0);
        if (ret < 0) {
            av_frame_free(&flush_frame);
            return ret;
        }

        ret = swr_convert(swr_ctx_, flush_frame->data, dst_nb_samples, nullptr, 0);
        if (ret < 0) {
            av_frame_free(&flush_frame);
            return ret;
        }
        if (ret == 0) {
            av_frame_free(&flush_frame);
            break;
        }

        flush_frame->nb_samples = ret;
        ret = av_audio_fifo_realloc(audio_fifo_, av_audio_fifo_size(audio_fifo_) + flush_frame->nb_samples);
        if (ret < 0) {
            av_frame_free(&flush_frame);
            return ret;
        }

        ret = av_audio_fifo_write(
            audio_fifo_,
            reinterpret_cast<void **>(flush_frame->data),
            flush_frame->nb_samples
        );
        av_frame_free(&flush_frame);
        if (ret < 0) return ret;
    }

    return 0;
}

int FFmpegMp4Transcoder::flushVideoPipeline() {
    int ret = avcodec_send_packet(video_dec_ctx_, nullptr);
    if (ret < 0 && ret != AVERROR_EOF) return ret;

    while ((ret = avcodec_receive_frame(video_dec_ctx_, video_dec_frame_)) >= 0) {
        if (video_dec_frame_->pts == AV_NOPTS_VALUE) {
            video_dec_frame_->pts = video_dec_frame_->best_effort_timestamp;
        }

        ret = processDecodedVideoFrame(video_dec_frame_);
        av_frame_unref(video_dec_frame_);
        if (ret < 0) return ret;
    }
    if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) return ret;

    ret = avcodec_send_frame(video_enc_ctx_, nullptr);
    if (ret < 0 && ret != AVERROR_EOF) return ret;

    return writeEncodedPackets(video_enc_ctx_, video_output_index_);
}

int FFmpegMp4Transcoder::flushAudioPipeline() {
    if (audio_copy_mode_) {
        return flushAudioCopyPacketBsf();
    }
    if (!audio_dec_ctx_ || !audio_enc_ctx_) return 0;

    int ret = avcodec_send_packet(audio_dec_ctx_, nullptr);
    if (ret < 0 && ret != AVERROR_EOF) return ret;

    while ((ret = avcodec_receive_frame(audio_dec_ctx_, audio_dec_frame_)) >= 0) {
        if (audio_dec_frame_->pts == AV_NOPTS_VALUE) {
            audio_dec_frame_->pts = audio_dec_frame_->best_effort_timestamp;
        }

        ret = processDecodedAudioFrame(audio_dec_frame_);
        av_frame_unref(audio_dec_frame_);
        if (ret < 0) return ret;
    }
    if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) return ret;

    ret = flushResamplerToFifo();
    if (ret < 0) return ret;

    ret = drainAudioFifo(1);
    if (ret < 0) return ret;

    ret = avcodec_send_frame(audio_enc_ctx_, nullptr);
    if (ret < 0 && ret != AVERROR_EOF) return ret;

    return writeEncodedPackets(audio_enc_ctx_, audio_output_index_);
}

int FFmpegMp4Transcoder::transcodeLoop() {
    AVPacket packet = {0};
    int ret = 0;

    while ((ret = av_read_frame(input_format_, &packet)) >= 0) {
        if (isCancelled()) break;

        ret = waitIfPaused();
        if (ret < 0) {
            av_packet_unref(&packet);
            return ret;
        }

        if (packet.stream_index == video_input_index_) {
            ret = processVideoPacket(&packet);
        } else if (audio_input_index_ >= 0 && packet.stream_index == audio_input_index_) {
            ret = audio_copy_mode_ ? processAudioPacketCopy(&packet) : processAudioPacket(&packet);
        }

        av_packet_unref(&packet);
        if (ret < 0) return ret;
    }

    av_packet_unref(&packet);
    if (ret != AVERROR_EOF && ret < 0) return ret;

    if (isCancelled()) return 0;

    ret = flushVideoPipeline();
    if (ret < 0) return ret;

    ret = flushAudioPipeline();
    if (ret < 0) return ret;

    ret = av_write_trailer(output_format_);
    if (ret < 0) return ret;

    logOutputFileProbeSummary();

    progress_.finish();
    return 0;
}

int FFmpegMp4Transcoder::transcode() {
    int ret = 0;

    if (input_path_.empty() || output_path_.empty()) return AVERROR(EINVAL);
    if (input_path_ == output_path_) return AVERROR(EINVAL);

    ret = openInput();
    if (ret < 0) {
        FFmpegCommonUtils::printError("Failed to open input", ret);
        return ret;
    }

    ret = openOutput();
    if (ret < 0) {
        FFmpegCommonUtils::printError("Failed to prepare output", ret);
        return ret;
    }

    ret = writeHeader();
    if (ret < 0) {
        FFmpegCommonUtils::printError("Failed to write mp4 header", ret);
        return ret;
    }

    ret = transcodeLoop();
    if (ret < 0) {
        FFmpegCommonUtils::printError("Transcode failed", ret);
    }
    return ret;
}

namespace {

class FFmpegBitstreamFilter {
public:
    FFmpegBitstreamFilter() : context_(nullptr) {}

    ~FFmpegBitstreamFilter() {
        if (context_) av_bsf_free(&context_);
    }

    int initialize(AVStream *stream, const char *name) {
        const AVBitStreamFilter *filter = av_bsf_get_by_name(name);
        if (!filter) return AVERROR_FILTER_NOT_FOUND;

        int ret = av_bsf_alloc(filter, &context_);
        if (ret < 0) return ret;

        ret = avcodec_parameters_copy(context_->par_in, stream->codecpar);
        if (ret < 0) {
            av_bsf_free(&context_);
            return ret;
        }

        context_->time_base_in = stream->time_base;
        ret = av_bsf_init(context_);
        if (ret < 0) {
            av_bsf_free(&context_);
            return ret;
        }
        return 0;
    }

    AVBSFContext *get() const {
        return context_;
    }

private:
    AVBSFContext *context_;
};

void rescalePacketTsForSeparateAudio(AVPacket *packet, AVRational source_time_base, AVRational destination_time_base) {
    if (!packet) return;
    if (source_time_base.num <= 0 || source_time_base.den <= 0) return;
    if (destination_time_base.num <= 0 || destination_time_base.den <= 0) return;
    av_packet_rescale_ts(packet, source_time_base, destination_time_base);
}

AVRational resolveSourceFrameRateForSeparateAudio(AVFormatContext *format_context, AVStream *stream) {
    AVRational frame_rate = av_guess_frame_rate(format_context, stream, nullptr);
    if ((frame_rate.num <= 0 || frame_rate.den <= 0) && stream) {
        frame_rate = stream->avg_frame_rate;
    }
    if ((frame_rate.num <= 0 || frame_rate.den <= 0) && stream) {
        frame_rate = stream->r_frame_rate;
    }
    if (frame_rate.num <= 0 || frame_rate.den <= 0) {
        frame_rate = AVRational{25, 1};
    }
    return frame_rate;
}

int frameRateToGopSizeForSeparateAudio(AVRational frame_rate) {
    if (frame_rate.num <= 0 || frame_rate.den <= 0) return 25;

    const int gop_size = static_cast<int>(
        av_rescale_rnd(frame_rate.num, 1, frame_rate.den, AV_ROUND_NEAR_INF)
    );
    return gop_size > 0 ? gop_size : 25;
}

int64_t streamStartTimeUsForSeparateAudio(AVFormatContext *format_context, AVStream *stream) {
    if (stream && stream->start_time != AV_NOPTS_VALUE) {
        return av_rescale_q(stream->start_time, stream->time_base, AV_TIME_BASE_Q);
    }
    if (format_context && format_context->start_time != AV_NOPTS_VALUE) {
        return format_context->start_time;
    }
    return AV_NOPTS_VALUE;
}

int64_t streamDurationUsForSeparateAudio(AVFormatContext *format_context, AVStream *stream) {
    if (stream && stream->duration > 0 && stream->duration != AV_NOPTS_VALUE) {
        return av_rescale_q(stream->duration, stream->time_base, AV_TIME_BASE_Q);
    }
    if (format_context) {
        const int64_t duration = FFmpegCommonUtils::getDurationInUsSafe(format_context);
        if (duration > 0) return duration;
    }
    return 0;
}

bool isStillImageCodecForSeparateAudio(enum AVCodecID codec_id) {
    switch (codec_id) {
        case AV_CODEC_ID_MJPEG:
        case AV_CODEC_ID_LJPEG:
        case AV_CODEC_ID_JPEGLS:
        case AV_CODEC_ID_PNG:
        case AV_CODEC_ID_BMP:
        case AV_CODEC_ID_GIF:
        case AV_CODEC_ID_TIFF:
        case AV_CODEC_ID_WEBP:
        case AV_CODEC_ID_JPEG2000:
        case AV_CODEC_ID_JPEGXL:
        case AV_CODEC_ID_PPM:
        case AV_CODEC_ID_PGM:
        case AV_CODEC_ID_PGMYUV:
        case AV_CODEC_ID_PBM:
        case AV_CODEC_ID_PAM:
        case AV_CODEC_ID_PCX:
        case AV_CODEC_ID_SGI:
        case AV_CODEC_ID_SVG:
            return true;
        default:
            return false;
    }
}

bool formatNameLooksLikeImageForSeparateAudio(const AVInputFormat *input_format) {
    if (!input_format || !input_format->name) return false;

    const char *name = input_format->name;
    return strstr(name, "image2") ||
        strstr(name, "png_pipe") ||
        strstr(name, "jpeg_pipe") ||
        strstr(name, "webp_pipe") ||
        strstr(name, "gif");
}

bool looksLikeStillImageInputForSeparateAudio(AVFormatContext *format_context, AVStream *stream) {
    if (!stream || !stream->codecpar) return false;

    if (formatNameLooksLikeImageForSeparateAudio(format_context ? format_context->iformat : nullptr)) {
        return true;
    }

    return isStillImageCodecForSeparateAudio(stream->codecpar->codec_id) &&
        streamDurationUsForSeparateAudio(format_context, stream) <= 0;
}

int64_t minPositiveDurationUs(int64_t lhs, int64_t rhs) {
    if (lhs > 0 && rhs > 0) return lhs < rhs ? lhs : rhs;
    if (lhs > 0) return lhs;
    if (rhs > 0) return rhs;
    return 0;
}

int64_t packetTimestamp(const AVPacket *packet) {
    if (!packet) return AV_NOPTS_VALUE;
    // 进度估算优先使用显示时间戳 pts；缺失时退回到解码顺序时间戳 dts。
    return packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
}

// 统一把输出路径扩展名转成小写，供 muxer 选择逻辑复用。
std::string lowerPathExtension(const std::string &path) {
    const std::string::size_type dot = path.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= path.size()) return std::string();

    std::string extension = path.substr(dot + 1);
    for (char &ch : extension) {
        ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    }
    return extension;
}

// 某些纯音频扩展名仅靠 FFmpeg 自动探测不稳定，这里按后缀显式指定 muxer 名称。
const char *audioExtractionMuxerNameForPath(const std::string &output_path) {
    const std::string extension = lowerPathExtension(output_path);
    if (extension.empty()) return nullptr;

    if (extension == "m4a" || extension == "m4b" || extension == "m4r" || extension == "mp4") {
        return "mp4";
    }
    if (extension == "aac") return "adts";
    if (extension == "mp3") return "mp3";
    if (extension == "flac") return "flac";
    if (extension == "ogg") return "ogg";
    if (extension == "opus") return "opus";
    if (extension == "ac3") return "ac3";
    if (extension == "wav") return "wav";
    if (extension == "mka" || extension == "mkv") return "matroska";
    return nullptr;
}

// MP4/MOV 家族不能直接保留 AAC 的 ADTS 帧头，需要改写成 AudioSpecificConfig extradata。
bool outputFormatNeedsAacAdtsToAsc(const AVOutputFormat *output_format) {
    if (!output_format || !output_format->name) return false;

    const char *format_name = output_format->name;
    return strcmp(format_name, "mp4") == 0 ||
        strcmp(format_name, "mov") == 0 ||
        strcmp(format_name, "ipod") == 0 ||
        strcmp(format_name, "3gp") == 0 ||
        strcmp(format_name, "3g2") == 0 ||
        strcmp(format_name, "psp") == 0 ||
        strcmp(format_name, "ismv") == 0 ||
        strcmp(format_name, "f4v") == 0;
}

// 纯抽音频场景的轻量级 remuxer：只复制 packet，必要时挂一个 AAC bitstream filter。
class FFmpegAudioExtractor {
public:
    // 保存一次音频抽取任务的入参、回调和外部暂停/取消状态。
    FFmpegAudioExtractor(
        const char *input_path,
        const char *output_path,
        void (*progress_cb)(int percentage, void *user_data),
        void *user_data,
        volatile int *cancel_flag,
        volatile int *pause_flag
    );
    // 释放输入/输出封装上下文和底层 IO。
    ~FFmpegAudioExtractor();

    // 执行 open input -> open output -> write header -> remux packets -> write trailer 全流程。
    int extract();

private:
    // 释放 demuxer、muxer、输出文件句柄和派生状态。
    void cleanup();
    // 轮询外部取消标记，供主循环和暂停等待复用。
    bool isCancelled() const;
    // 轮询外部暂停标记。
    bool isPaused() const;
    // 在暂停期间阻塞当前线程，并响应取消请求。
    int waitIfPaused();
    // 判断当前输出容器是否需要把 AAC 的 ADTS 头改写成 ASC extradata。
    bool shouldApplyAacAdtsToAsc() const;

    // 打开输入媒体、探测流信息并选出最佳音频流。
    int openInput();
    // 创建输出容器和输出音频流，必要时挂载 aac_adtstoasc bitstream filter。
    int openOutput();
    // 向 muxer 写文件头，固化流布局和 metadata。
    int writeHeader();
    // 逐包读取输入音频并重封装到输出容器。
    int processPackets();
    // 在输入结束后把 bitstream filter 内部缓存的尾包全部刷出。
    int flushBitstreamFilter();
    // 把 packet 的时间戳改到输出时间基后写给 muxer。
    int writePacket(AVPacket *packet, AVRational source_time_base);

    std::string input_path_;
    std::string output_path_;
    void (*progress_cb_)(int percentage, void *user_data);
    void *user_data_;
    volatile int *cancel_flag_;
    volatile int *pause_flag_;
    AVFormatContext *input_format_;
    AVFormatContext *output_format_;
    int audio_input_index_;
    int audio_output_index_;
    bool pause_observed_;
    FFmpegBitstreamFilter audio_bsf_;
    FFmpegProgressTracker progress_;
};

FFmpegAudioExtractor::FFmpegAudioExtractor(
    const char *input_path,
    const char *output_path,
    void (*progress_cb)(int percentage, void *user_data),
    void *user_data,
    volatile int *cancel_flag,
    volatile int *pause_flag
) : input_path_(input_path ? input_path : ""),
    output_path_(output_path ? output_path : ""),
    progress_cb_(progress_cb),
    user_data_(user_data),
    cancel_flag_(cancel_flag),
    pause_flag_(pause_flag),
    input_format_(nullptr),
    output_format_(nullptr),
    audio_input_index_(-1),
    audio_output_index_(-1),
    pause_observed_(false),
    progress_() {
    FFmpegCommonUtils::installPlatformLogBridge();
}

FFmpegAudioExtractor::~FFmpegAudioExtractor() {
    cleanup();
}

void FFmpegAudioExtractor::cleanup() {
    // avformat_close_input 会关闭 demuxer 并释放 input_format_ 持有的内部状态。
    if (input_format_) avformat_close_input(&input_format_);
    if (output_format_) {
        // avio_closep 关闭输出文件；avformat_free_context 释放 muxer 上下文本身。
        if (output_format_->pb) avio_closep(&output_format_->pb);
        avformat_free_context(output_format_);
    }

    input_format_ = nullptr;
    output_format_ = nullptr;
    audio_input_index_ = -1;
    audio_output_index_ = -1;
    pause_observed_ = false;
}

bool FFmpegAudioExtractor::isCancelled() const {
    return cancel_flag_ && *cancel_flag_;
}

bool FFmpegAudioExtractor::isPaused() const {
    return pause_flag_ && *pause_flag_;
}

int FFmpegAudioExtractor::waitIfPaused() {
    while (isPaused()) {
        if (!pause_observed_) {
            pause_observed_ = true;
            AWESOME_FF_LOGI("Audio extraction paused.");
        }
        if (isCancelled()) return AVERROR_EXIT;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (pause_observed_) {
        pause_observed_ = false;
        AWESOME_FF_LOGI("Audio extraction resumed.");
    }
    return 0;
}

bool FFmpegAudioExtractor::shouldApplyAacAdtsToAsc() const {
    if (!input_format_ || !output_format_ || audio_input_index_ < 0) return false;

    // 输入 AAC 没有 extradata 时，通常说明每个 packet 里仍带着 ADTS 头，需要 remux 前转成 ASC。
    AVStream *input_stream = input_format_->streams[audio_input_index_];
    return input_stream &&
        input_stream->codecpar &&
        input_stream->codecpar->codec_id == AV_CODEC_ID_AAC &&
        input_stream->codecpar->extradata_size == 0 &&
        outputFormatNeedsAacAdtsToAsc(output_format_->oformat);
}

int FFmpegAudioExtractor::openInput() {
    // avformat_open_input 负责探测输入容器并创建 demuxer 上下文。
    int ret = avformat_open_input(&input_format_, input_path_.c_str(), nullptr, nullptr);
    if (ret < 0) return ret;

    // avformat_find_stream_info 会预读一小段数据，补齐 codecpar/time_base/duration 等流信息。
    ret = avformat_find_stream_info(input_format_, nullptr);
    if (ret < 0) return ret;

    // av_find_best_stream 让 FFmpeg 帮我们挑出最合适的一条音频流。
    audio_input_index_ = av_find_best_stream(input_format_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audio_input_index_ < 0) return audio_input_index_;

    progress_.reset(
        streamDurationUsForSeparateAudio(input_format_, input_format_->streams[audio_input_index_]),
        progress_cb_,
        user_data_
    );
    return 0;
}

int FFmpegAudioExtractor::openOutput() {
    // 先尝试让 FFmpeg 根据输出路径自动推断 muxer；失败时再按扩展名兜底。
    int ret = avformat_alloc_output_context2(&output_format_, nullptr, nullptr, output_path_.c_str());
    if ((ret < 0 || !output_format_)) {
        if (output_format_) {
            avformat_free_context(output_format_);
            output_format_ = nullptr;
        }

        // 例如 m4a/aac/mp3 这类目标，经常需要显式指定 muxer 名称才能稳定建出容器。
        const char *forced_muxer_name = audioExtractionMuxerNameForPath(output_path_);
        if (forced_muxer_name) {
            ret = avformat_alloc_output_context2(
                &output_format_,
                nullptr,
                forced_muxer_name,
                output_path_.c_str()
            );
        }
    }
    if (ret < 0 || !output_format_) {
        return ret < 0 ? ret : AVERROR(EINVAL);
    }

    AVStream *input_stream = input_format_->streams[audio_input_index_];
    // avformat_new_stream 在输出容器中注册一条音频流，后续所有输出 packet 都写到这里。
    AVStream *output_stream = avformat_new_stream(output_format_, nullptr);
    if (!output_stream) return AVERROR(ENOMEM);

    if (shouldApplyAacAdtsToAsc()) {
        // aac_adtstoasc 会把每个包里的 ADTS 头转成 mp4/mov 需要的 codec extradata。
        ret = audio_bsf_.initialize(input_stream, "aac_adtstoasc");
        if (ret < 0) return ret;

        // 把 bitstream filter 输出侧的 codecpar 复制给 muxer，保证头信息与过滤后数据一致。
        ret = avcodec_parameters_copy(output_stream->codecpar, audio_bsf_.get()->par_out);
        if (ret < 0) return ret;
        output_stream->time_base = audio_bsf_.get()->time_base_out;
    } else {
        // 不做过滤时直接复制输入音频流参数，保持“只抽取不转码”。
        ret = avcodec_parameters_copy(output_stream->codecpar, input_stream->codecpar);
        if (ret < 0) return ret;
        output_stream->time_base = input_stream->time_base;
    }

    // 置 0 让 muxer 重新生成 codec_tag，避免把源容器里的 tag 原样带进新容器。
    output_stream->codecpar->codec_tag = 0;
    output_stream->disposition = input_stream->disposition;
    av_dict_copy(&output_stream->metadata, input_stream->metadata, 0);
    av_dict_copy(&output_format_->metadata, input_format_->metadata, 0);
    audio_output_index_ = output_stream->index;

    if (!(output_format_->oformat->flags & AVFMT_NOFILE)) {
        // avio_open 打开最终输出文件，让后续 write_header/write_frame/write_trailer 可以落盘。
        ret = avio_open(&output_format_->pb, output_path_.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) return ret;
    }

    return 0;
}

int FFmpegAudioExtractor::writeHeader() {
    // avformat_write_header 会把容器头和流描述写出去，并最终校验 muxer 参数。
    return avformat_write_header(output_format_, nullptr);
}

int FFmpegAudioExtractor::writePacket(AVPacket *packet, AVRational source_time_base) {
    if (!packet || !output_format_ || audio_output_index_ < 0) return AVERROR(EINVAL);

    AVStream *output_stream = output_format_->streams[audio_output_index_];
    // remux 前先把 pts/dts 转到输出流时间基，避免 muxer 看到跨容器的旧时间戳。
    rescalePacketTsForSeparateAudio(packet, source_time_base, output_stream->time_base);
    packet->stream_index = audio_output_index_;
    // 原始文件偏移对新容器没有意义，置 -1 交给 muxer 重新管理。
    packet->pos = -1;
    // av_interleaved_write_frame 负责按 muxer 规则排序并写出 packet。
    return av_interleaved_write_frame(output_format_, packet);
}

int FFmpegAudioExtractor::processPackets() {
    if (!input_format_ || audio_input_index_ < 0) return AVERROR(EINVAL);

    AVStream *input_stream = input_format_->streams[audio_input_index_];
    AVPacket packet = {0};
    int ret = 0;

    // av_read_frame 以 demux 后的 packet 为粒度读取输入；这里不解码，只筛选目标音频流。
    while ((ret = av_read_frame(input_format_, &packet)) >= 0) {
        ret = waitIfPaused();
        if (ret < 0) {
            av_packet_unref(&packet);
            return ret;
        }
        if (isCancelled()) {
            av_packet_unref(&packet);
            return 0;
        }

        if (packet.stream_index != audio_input_index_) {
            av_packet_unref(&packet);
            continue;
        }

        progress_.update(packetTimestamp(&packet), input_stream->time_base);

        if (audio_bsf_.get()) {
            // 把原始 AAC packet 送入 bitstream filter 队列，后面再逐个取出转换后的包。
            ret = av_bsf_send_packet(audio_bsf_.get(), &packet);
            // packet 可能持有 demuxer 返回的引用，无论是否继续使用都要在当前轮释放。
            av_packet_unref(&packet);
            if (ret < 0) return ret;

            // 一个输入包可能对应 0..N 个输出包，循环拉取直到 filter 暂时没有更多数据。
            while ((ret = av_bsf_receive_packet(audio_bsf_.get(), &packet)) == 0) {
                const int write_ret = writePacket(&packet, audio_bsf_.get()->time_base_out);
                av_packet_unref(&packet);
                if (write_ret < 0) return write_ret;
            }

            if (ret == AVERROR(EAGAIN)) continue;
            if (ret == AVERROR_EOF) return 0;
            if (ret < 0) return ret;
            continue;
        }

        // 不走 bitstream filter 时直接 remux 当前音频包。
        ret = writePacket(&packet, input_stream->time_base);
        av_packet_unref(&packet);
        if (ret < 0) return ret;
    }

    if (ret != AVERROR_EOF && ret < 0) return ret;
    return flushBitstreamFilter();
}

int FFmpegAudioExtractor::flushBitstreamFilter() {
    if (!audio_bsf_.get()) return 0;

    int ret = waitIfPaused();
    if (ret < 0) return ret;
    if (isCancelled()) return 0;

    // 送入 nullptr 进入 drain 模式，让 filter 把内部缓存的尾包全部吐出来。
    ret = av_bsf_send_packet(audio_bsf_.get(), nullptr);
    if (ret < 0) return ret;

    AVPacket packet = {0};
    while ((ret = av_bsf_receive_packet(audio_bsf_.get(), &packet)) == 0) {
        // 冲刷阶段拿到的仍然是普通 packet，写完后一样需要 unref。
        const int write_ret = writePacket(&packet, audio_bsf_.get()->time_base_out);
        av_packet_unref(&packet);
        if (write_ret < 0) return write_ret;
    }

    if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) return 0;
    return ret;
}

int FFmpegAudioExtractor::extract() {
    if (input_path_.empty() || output_path_.empty()) return AVERROR(EINVAL);
    if (input_path_ == output_path_) return AVERROR(EINVAL);

    // 整体流程保持为“打开输入 -> 准备输出 -> 写头 -> 按包重封装 -> 写尾”。
    int ret = openInput();
    if (ret < 0) {
        FFmpegCommonUtils::printError("Failed to open input for audio extraction", ret);
        return ret;
    }

    ret = openOutput();
    if (ret < 0) {
        FFmpegCommonUtils::printError("Failed to prepare audio extraction output", ret);
        return ret;
    }

    ret = waitIfPaused();
    if (ret < 0) return ret;

    ret = writeHeader();
    if (ret < 0) {
        FFmpegCommonUtils::printError("Failed to write audio extraction header", ret);
        return ret;
    }

    ret = processPackets();
    if (ret < 0) {
        FFmpegCommonUtils::printError("Failed to extract audio packets", ret);
        return ret;
    }

    if (isCancelled()) return 0;

    ret = waitIfPaused();
    if (ret < 0) return ret;

    // av_write_trailer 收尾容器索引和尾信息；只有完整写完 trailer，目标文件才算封装完成。
    ret = av_write_trailer(output_format_);
    if (ret < 0) {
        FFmpegCommonUtils::printError("Failed to write audio extraction trailer", ret);
        return ret;
    }

    progress_.finish();
    return 0;
}

class FFmpegMp4SeparateAudioTranscoder {
public:
    FFmpegMp4SeparateAudioTranscoder(
        const char *visual_input_path,
        const char *audio_input_path,
        const char *output_path,
        const FFmpegMp4TranscodeConfig *config,
        void (*progress_cb)(int percentage, void *user_data),
        void *user_data,
        volatile int *cancel_flag,
        volatile int *pause_flag
    );
    ~FFmpegMp4SeparateAudioTranscoder();

    int transcode();

private:
    void cleanup();
    bool isCancelled() const;
    bool isPaused() const;
    int waitIfPaused();
    bool shouldCopyAudio() const;
    int64_t normalizePts(
        int64_t pts,
        AVRational source_time_base,
        AVRational destination_time_base,
        int64_t start_time_us,
        int64_t output_offset_pts = 0
    ) const;
    int64_t targetDurationPts(AVRational time_base) const;
    bool isPastTarget(int64_t pts, AVRational time_base) const;
    void clampPacketDurationToTarget(AVPacket *packet, AVRational time_base) const;

    int openInputs();
    int openVisualInput();
    int openAudioInput();
    int initializeImageDecoder();
    int initializeAudioDecoder();
    int loadStillImageFrame();

    int openOutput();
    int initializeVideoCopyStream();
    int initializeImageVideoEncoder();
    int initializeAudioCopyStream();
    int initializeAudioTranscodePipeline();
    int writeHeader();
    int writeEncoderPackets(AVCodecContext *encoder_context, int output_stream_index);

    int processVisualVideoPackets();
    int processStillImageVideo();
    int flushStillImageVideo();

    int processAudioPackets();
    int writeAudioCopyPacket(AVPacket *packet, AVRational source_time_base);
    int processAudioTranscodePacket(AVPacket *packet);
    int processDecodedAudioFrame(AVFrame *frame);
    int drainAudioFifo(int flush_last_frame);
    int flushResamplerToFifo();
    int flushAudioTranscodePipeline();

    std::string visual_input_path_;
    std::string audio_input_path_;
    std::string output_path_;
    std::string video_preset_;
    std::string video_profile_;
    std::string video_level_;
    int video_crf_;
    int audio_bitrate_;
    int faststart_;
    int frame_rate_;
    int scale_width_;
    int scale_height_;
    void (*progress_cb_)(int percentage, void *user_data);
    void *user_data_;
    volatile int *cancel_flag_;
    volatile int *pause_flag_;

    AVFormatContext *visual_input_format_;
    AVFormatContext *audio_input_format_;
    AVFormatContext *output_format_;
    AVCodecContext *image_dec_ctx_;
    AVCodecContext *image_video_enc_ctx_;
    AVCodecContext *audio_dec_ctx_;
    AVCodecContext *audio_enc_ctx_;
    SwsContext *sws_ctx_;
    SwrContext *swr_ctx_;
    AVAudioFifo *audio_fifo_;
    AVFrame *image_dec_frame_;
    AVFrame *image_source_frame_;
    AVFrame *image_video_frame_;
    AVFrame *audio_dec_frame_;
    AVPacket *enc_packet_;
    int visual_input_index_;
    int audio_input_index_;
    int video_output_index_;
    int audio_output_index_;
    int64_t visual_start_time_us_;
    int64_t audio_start_time_us_;
    int64_t visual_duration_us_;
    int64_t audio_duration_us_;
    int64_t target_duration_us_;
    int64_t audio_next_pts_;
    bool visual_is_image_;
    bool audio_copy_mode_;
    bool pause_observed_;
    FFmpegBitstreamFilter audio_bsf_;
    FFmpegProgressTracker progress_;
};

FFmpegMp4SeparateAudioTranscoder::FFmpegMp4SeparateAudioTranscoder(
    const char *visual_input_path,
    const char *audio_input_path,
    const char *output_path,
    const FFmpegMp4TranscodeConfig *config,
    void (*progress_cb)(int percentage, void *user_data),
    void *user_data,
    volatile int *cancel_flag,
    volatile int *pause_flag
) : visual_input_path_(visual_input_path ? visual_input_path : ""),
    audio_input_path_(audio_input_path ? audio_input_path : ""),
    output_path_(output_path ? output_path : ""),
    video_preset_(config && config->preset && config->preset[0] ? config->preset : "fast"),
    video_profile_(config && config->profile && config->profile[0] ? config->profile : "high"),
    video_level_(config && config->level && config->level[0] ? config->level : "4.2"),
    video_crf_(config && config->crf > 0 ? config->crf : 23),
    audio_bitrate_(config && config->audio_bitrate > 0 ? config->audio_bitrate : 128000),
    faststart_(!config || config->faststart >= 0 ? 1 : 0),
    frame_rate_(config && config->frame_rate > 0 ? config->frame_rate : 0),
    scale_width_(config && config->scale_width > 0 ? config->scale_width : 0),
    scale_height_(config && config->scale_height > 0 ? config->scale_height : 0),
    progress_cb_(progress_cb),
    user_data_(user_data),
    cancel_flag_(cancel_flag),
    pause_flag_(pause_flag),
    visual_input_format_(nullptr),
    audio_input_format_(nullptr),
    output_format_(nullptr),
    image_dec_ctx_(nullptr),
    image_video_enc_ctx_(nullptr),
    audio_dec_ctx_(nullptr),
    audio_enc_ctx_(nullptr),
    sws_ctx_(nullptr),
    swr_ctx_(nullptr),
    audio_fifo_(nullptr),
    image_dec_frame_(nullptr),
    image_source_frame_(nullptr),
    image_video_frame_(nullptr),
    audio_dec_frame_(nullptr),
    enc_packet_(nullptr),
    visual_input_index_(-1),
    audio_input_index_(-1),
    video_output_index_(-1),
    audio_output_index_(-1),
    visual_start_time_us_(AV_NOPTS_VALUE),
    audio_start_time_us_(AV_NOPTS_VALUE),
    visual_duration_us_(0),
    audio_duration_us_(0),
    target_duration_us_(0),
    audio_next_pts_(AV_NOPTS_VALUE),
    visual_is_image_(false),
    audio_copy_mode_(false),
    pause_observed_(false),
    progress_() {
}

FFmpegMp4SeparateAudioTranscoder::~FFmpegMp4SeparateAudioTranscoder() {
    cleanup();
}

void FFmpegMp4SeparateAudioTranscoder::cleanup() {
    if (enc_packet_) av_packet_free(&enc_packet_);
    if (audio_dec_frame_) av_frame_free(&audio_dec_frame_);
    if (image_video_frame_) av_frame_free(&image_video_frame_);
    if (image_source_frame_) av_frame_free(&image_source_frame_);
    if (image_dec_frame_) av_frame_free(&image_dec_frame_);
    if (audio_fifo_) av_audio_fifo_free(audio_fifo_);
    if (swr_ctx_) swr_free(&swr_ctx_);
    if (sws_ctx_) sws_freeContext(sws_ctx_);
    if (audio_enc_ctx_) avcodec_free_context(&audio_enc_ctx_);
    if (audio_dec_ctx_) avcodec_free_context(&audio_dec_ctx_);
    if (image_video_enc_ctx_) avcodec_free_context(&image_video_enc_ctx_);
    if (image_dec_ctx_) avcodec_free_context(&image_dec_ctx_);
    if (visual_input_format_) avformat_close_input(&visual_input_format_);
    if (audio_input_format_) avformat_close_input(&audio_input_format_);
    if (output_format_) {
        if (output_format_->pb) avio_closep(&output_format_->pb);
        avformat_free_context(output_format_);
    }

    visual_input_format_ = nullptr;
    audio_input_format_ = nullptr;
    output_format_ = nullptr;
    image_dec_ctx_ = nullptr;
    image_video_enc_ctx_ = nullptr;
    audio_dec_ctx_ = nullptr;
    audio_enc_ctx_ = nullptr;
    sws_ctx_ = nullptr;
    swr_ctx_ = nullptr;
    audio_fifo_ = nullptr;
    image_dec_frame_ = nullptr;
    image_source_frame_ = nullptr;
    image_video_frame_ = nullptr;
    audio_dec_frame_ = nullptr;
    enc_packet_ = nullptr;
    visual_input_index_ = -1;
    audio_input_index_ = -1;
    video_output_index_ = -1;
    audio_output_index_ = -1;
    visual_start_time_us_ = AV_NOPTS_VALUE;
    audio_start_time_us_ = AV_NOPTS_VALUE;
    visual_duration_us_ = 0;
    audio_duration_us_ = 0;
    target_duration_us_ = 0;
    audio_next_pts_ = AV_NOPTS_VALUE;
    visual_is_image_ = false;
    audio_copy_mode_ = false;
    pause_observed_ = false;
}

bool FFmpegMp4SeparateAudioTranscoder::isCancelled() const {
    return cancel_flag_ && *cancel_flag_;
}

bool FFmpegMp4SeparateAudioTranscoder::isPaused() const {
    return pause_flag_ && *pause_flag_;
}

int FFmpegMp4SeparateAudioTranscoder::waitIfPaused() {
    while (isPaused()) {
        if (!pause_observed_) {
            pause_observed_ = true;
            AWESOME_FF_LOGI("Transcode paused.");
        }
        if (isCancelled()) return AVERROR_EXIT;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (pause_observed_) {
        pause_observed_ = false;
        AWESOME_FF_LOGI("Transcode resumed.");
    }
    return 0;
}

bool FFmpegMp4SeparateAudioTranscoder::shouldCopyAudio() const {
    const bool output_is_mp4 = true;
    const bool no_audio_filter = true;
    return audio_input_format_ &&
        audio_input_index_ >= 0 &&
        audio_input_format_->streams[audio_input_index_]->codecpar &&
        audio_input_format_->streams[audio_input_index_]->codecpar->codec_id == AV_CODEC_ID_AAC &&
        output_is_mp4 &&
        no_audio_filter;
}

int64_t FFmpegMp4SeparateAudioTranscoder::normalizePts(
    int64_t pts,
    AVRational source_time_base,
    AVRational destination_time_base,
    int64_t start_time_us,
    int64_t output_offset_pts
) const {
    if (pts == AV_NOPTS_VALUE) return AV_NOPTS_VALUE;

    int64_t normalized = av_rescale_q(pts, source_time_base, destination_time_base);
    if (start_time_us != AV_NOPTS_VALUE) {
        normalized -= av_rescale_q(start_time_us, AV_TIME_BASE_Q, destination_time_base);
    }
    normalized += output_offset_pts;
    if (normalized < 0) normalized = 0;
    return normalized;
}

int64_t FFmpegMp4SeparateAudioTranscoder::targetDurationPts(AVRational time_base) const {
    if (target_duration_us_ <= 0) return AV_NOPTS_VALUE;
    if (time_base.num <= 0 || time_base.den <= 0) return AV_NOPTS_VALUE;
    return av_rescale_q(target_duration_us_, AV_TIME_BASE_Q, time_base);
}

bool FFmpegMp4SeparateAudioTranscoder::isPastTarget(int64_t pts, AVRational time_base) const {
    const int64_t end_pts = targetDurationPts(time_base);
    return end_pts != AV_NOPTS_VALUE && pts != AV_NOPTS_VALUE && pts >= end_pts;
}

void FFmpegMp4SeparateAudioTranscoder::clampPacketDurationToTarget(AVPacket *packet, AVRational time_base) const {
    if (!packet || packet->duration <= 0) return;

    const int64_t end_pts = targetDurationPts(time_base);
    const int64_t start_pts = packetTimestamp(packet);
    if (end_pts == AV_NOPTS_VALUE || start_pts == AV_NOPTS_VALUE) return;

    const int64_t packet_end_pts = start_pts + packet->duration;
    if (packet_end_pts > end_pts) {
        const int64_t clipped_duration = end_pts - start_pts;
        packet->duration = clipped_duration > 0 ? clipped_duration : 0;
    }
}

int FFmpegMp4SeparateAudioTranscoder::openInputs() {
    int ret = openVisualInput();
    if (ret < 0) return ret;

    ret = openAudioInput();
    if (ret < 0) return ret;

    target_duration_us_ = visual_is_image_
        ? audio_duration_us_
        : minPositiveDurationUs(visual_duration_us_, audio_duration_us_);

    if (visual_is_image_ && target_duration_us_ <= 0) {
        return AVERROR(EINVAL);
    }

    progress_.reset(target_duration_us_, progress_cb_, user_data_);
    return 0;
}

int FFmpegMp4SeparateAudioTranscoder::openVisualInput() {
    int ret = avformat_open_input(&visual_input_format_, visual_input_path_.c_str(), nullptr, nullptr);
    if (ret < 0) return ret;

    ret = avformat_find_stream_info(visual_input_format_, nullptr);
    if (ret < 0) return ret;

    visual_input_index_ = av_find_best_stream(visual_input_format_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (visual_input_index_ < 0) return visual_input_index_;

    AVStream *visual_stream = visual_input_format_->streams[visual_input_index_];
    visual_start_time_us_ = streamStartTimeUsForSeparateAudio(visual_input_format_, visual_stream);
    visual_duration_us_ = streamDurationUsForSeparateAudio(visual_input_format_, visual_stream);
    visual_is_image_ = looksLikeStillImageInputForSeparateAudio(visual_input_format_, visual_stream);

    if (visual_is_image_) {
        ret = initializeImageDecoder();
        if (ret < 0) return ret;

        ret = loadStillImageFrame();
        if (ret < 0) return ret;
    }

    return 0;
}

int FFmpegMp4SeparateAudioTranscoder::openAudioInput() {
    int ret = avformat_open_input(&audio_input_format_, audio_input_path_.c_str(), nullptr, nullptr);
    if (ret < 0) return ret;

    ret = avformat_find_stream_info(audio_input_format_, nullptr);
    if (ret < 0) return ret;

    audio_input_index_ = av_find_best_stream(audio_input_format_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audio_input_index_ < 0) return audio_input_index_;

    AVStream *audio_stream = audio_input_format_->streams[audio_input_index_];
    audio_start_time_us_ = streamStartTimeUsForSeparateAudio(audio_input_format_, audio_stream);
    audio_duration_us_ = streamDurationUsForSeparateAudio(audio_input_format_, audio_stream);
    audio_copy_mode_ = shouldCopyAudio();

    if (!audio_copy_mode_) {
        ret = initializeAudioDecoder();
        if (ret < 0) return ret;
    }

    return 0;
}

int FFmpegMp4SeparateAudioTranscoder::initializeImageDecoder() {
    AVStream *input_stream = visual_input_format_->streams[visual_input_index_];
    const AVCodec *decoder = avcodec_find_decoder(input_stream->codecpar->codec_id);
    if (!decoder) return AVERROR_DECODER_NOT_FOUND;

    image_dec_ctx_ = avcodec_alloc_context3(decoder);
    if (!image_dec_ctx_) return AVERROR(ENOMEM);

    int ret = avcodec_parameters_to_context(image_dec_ctx_, input_stream->codecpar);
    if (ret < 0) return ret;

    image_dec_ctx_->pkt_timebase = input_stream->time_base;
    return avcodec_open2(image_dec_ctx_, decoder, nullptr);
}

int FFmpegMp4SeparateAudioTranscoder::initializeAudioDecoder() {
    AVStream *input_stream = audio_input_format_->streams[audio_input_index_];
    const AVCodec *decoder = avcodec_find_decoder(input_stream->codecpar->codec_id);
    if (!decoder) return AVERROR_DECODER_NOT_FOUND;

    audio_dec_ctx_ = avcodec_alloc_context3(decoder);
    if (!audio_dec_ctx_) return AVERROR(ENOMEM);

    int ret = avcodec_parameters_to_context(audio_dec_ctx_, input_stream->codecpar);
    if (ret < 0) return ret;

    audio_dec_ctx_->pkt_timebase = input_stream->time_base;
    return avcodec_open2(audio_dec_ctx_, decoder, nullptr);
}

int FFmpegMp4SeparateAudioTranscoder::loadStillImageFrame() {
    int ret = 0;
    AVPacket packet = {0};

    image_dec_frame_ = av_frame_alloc();
    image_source_frame_ = av_frame_alloc();
    if (!image_dec_frame_ || !image_source_frame_) return AVERROR(ENOMEM);

    while ((ret = av_read_frame(visual_input_format_, &packet)) >= 0) {
        if (packet.stream_index != visual_input_index_) {
            av_packet_unref(&packet);
            continue;
        }

        ret = avcodec_send_packet(image_dec_ctx_, &packet);
        av_packet_unref(&packet);
        if (ret < 0) return ret;

        ret = avcodec_receive_frame(image_dec_ctx_, image_dec_frame_);
        if (ret == 0) {
            av_frame_move_ref(image_source_frame_, image_dec_frame_);
            av_frame_unref(image_dec_frame_);
            return 0;
        }
        if (ret != AVERROR(EAGAIN)) {
            return ret;
        }
    }

    av_packet_unref(&packet);
    if (ret != AVERROR_EOF && ret < 0) return ret;

    ret = avcodec_send_packet(image_dec_ctx_, nullptr);
    if (ret < 0 && ret != AVERROR_EOF) return ret;

    ret = avcodec_receive_frame(image_dec_ctx_, image_dec_frame_);
    if (ret < 0) return ret;

    av_frame_move_ref(image_source_frame_, image_dec_frame_);
    av_frame_unref(image_dec_frame_);
    return 0;
}

int FFmpegMp4SeparateAudioTranscoder::openOutput() {
    int ret = avformat_alloc_output_context2(&output_format_, nullptr, "mp4", output_path_.c_str());
    if (ret < 0) return ret;

    if (visual_is_image_) {
        ret = initializeImageVideoEncoder();
    } else {
        if (frame_rate_ > 0) {
            AWESOME_FF_LOGW(
                "Ignoring requested frame rate in separate-audio mode because the video input is copied without re-encoding."
            );
        }
        ret = initializeVideoCopyStream();
    }
    if (ret < 0) {
        FFmpegCommonUtils::printError("Failed to initialize visual output", ret);
        return ret;
    }

    if (audio_copy_mode_) {
        ret = initializeAudioCopyStream();
    } else {
        ret = initializeAudioTranscodePipeline();
    }
    if (ret < 0) {
        FFmpegCommonUtils::printError("Failed to initialize audio output", ret);
        return ret;
    }

    enc_packet_ = av_packet_alloc();
    if (!enc_packet_) return AVERROR(ENOMEM);

    if (!(output_format_->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&output_format_->pb, output_path_.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) return ret;
    }

    av_dict_copy(&output_format_->metadata, visual_input_format_->metadata, 0);
    return 0;
}

int FFmpegMp4SeparateAudioTranscoder::initializeVideoCopyStream() {
    AVStream *input_stream = visual_input_format_->streams[visual_input_index_];
    if (avformat_query_codec(output_format_->oformat, input_stream->codecpar->codec_id, FF_COMPLIANCE_NORMAL) <= 0) {
        return AVERROR(EINVAL);
    }

    AVStream *output_stream = avformat_new_stream(output_format_, nullptr);
    if (!output_stream) return AVERROR(ENOMEM);

    int ret = avcodec_parameters_copy(output_stream->codecpar, input_stream->codecpar);
    if (ret < 0) return ret;

    video_output_index_ = output_stream->index;
    output_stream->codecpar->codec_tag = 0;
    output_stream->time_base = input_stream->time_base;
    output_stream->avg_frame_rate = input_stream->avg_frame_rate;
    output_stream->sample_aspect_ratio = input_stream->sample_aspect_ratio;
    output_stream->disposition = input_stream->disposition;
    av_dict_copy(&output_stream->metadata, input_stream->metadata, 0);
    return FFmpegCommonUtils::copyDisplayMatrixSideData(input_stream, output_stream);
}

int FFmpegMp4SeparateAudioTranscoder::initializeImageVideoEncoder() {
    if (!image_source_frame_) return AVERROR(EINVAL);

    AVStream *input_stream = visual_input_format_->streams[visual_input_index_];
    AVStream *output_stream = nullptr;
    int ret = 0;
    int output_width = 0;
    int output_height = 0;

    const AVCodec *encoder = FFmpegCommonUtils::findPreferredVideoEncoder(AV_CODEC_ID_H264);
    if (!encoder) return AVERROR_ENCODER_NOT_FOUND;

    AVRational frame_rate = frame_rate_ > 0
        ? AVRational{frame_rate_, 1}
        : resolveSourceFrameRateForSeparateAudio(visual_input_format_, input_stream);

    ret = FFmpegCommonUtils::resolveOutputVideoSize(
        image_source_frame_->width,
        image_source_frame_->height,
        scale_width_,
        scale_height_,
        &output_width,
        &output_height
    );
    if (ret < 0) return ret;

    auto configureEncoderContext = [&](const AVCodec *candidate) -> int {
        if (image_video_enc_ctx_) avcodec_free_context(&image_video_enc_ctx_);

        image_video_enc_ctx_ = avcodec_alloc_context3(candidate);
        if (!image_video_enc_ctx_) return AVERROR(ENOMEM);

        image_video_enc_ctx_->codec_id = candidate->id;
        image_video_enc_ctx_->codec_type = AVMEDIA_TYPE_VIDEO;
        image_video_enc_ctx_->width = output_width;
        image_video_enc_ctx_->height = output_height;
        image_video_enc_ctx_->sample_aspect_ratio = input_stream->sample_aspect_ratio.num > 0 &&
                input_stream->sample_aspect_ratio.den > 0
            ? input_stream->sample_aspect_ratio
            : AVRational{1, 1};
        image_video_enc_ctx_->pix_fmt = FFmpegCommonUtils::chooseVideoPixelFormat(candidate);
        image_video_enc_ctx_->time_base = av_inv_q(frame_rate);
        if (image_video_enc_ctx_->time_base.num <= 0 || image_video_enc_ctx_->time_base.den <= 0) {
            image_video_enc_ctx_->time_base = AVRational{1, 25};
        }
        image_video_enc_ctx_->framerate = frame_rate;
        image_video_enc_ctx_->bit_rate = 2 * 1000 * 1000;
        image_video_enc_ctx_->gop_size = frameRateToGopSizeForSeparateAudio(frame_rate);
        image_video_enc_ctx_->max_b_frames = 0;
        image_video_enc_ctx_->color_range = image_source_frame_->color_range;
        image_video_enc_ctx_->color_primaries = image_source_frame_->color_primaries;
        image_video_enc_ctx_->color_trc = image_source_frame_->color_trc;
        image_video_enc_ctx_->colorspace = image_source_frame_->colorspace;
        image_video_enc_ctx_->profile = FF_PROFILE_H264_HIGH;
        {
            const int parsed_level = FFmpegCommonUtils::parseH264Level(video_level_);
            if (parsed_level > 0) image_video_enc_ctx_->level = parsed_level;
        }

        if (candidate->capabilities & AV_CODEC_CAP_EXPERIMENTAL) {
            image_video_enc_ctx_->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
        }

        if ((output_format_->oformat->flags & AVFMT_GLOBALHEADER) &&
            !FFmpegCommonUtils::isMediaCodecEncoder(candidate)) {
            image_video_enc_ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }

        FFmpegCommonUtils::sanitizeVideoEncoderContextDefaults(image_video_enc_ctx_, candidate);
        return 0;
    };

    auto openEncoder = [&](const AVCodec *candidate) -> int {
        AVDictionary *codec_options = nullptr;
        char crf_buffer[16];

        av_dict_set(&codec_options, "profile", video_profile_.c_str(), 0);
        av_dict_set(&codec_options, "level", video_level_.c_str(), 0);
        if (FFmpegCommonUtils::isVideoToolboxEncoder(candidate)) {
            av_dict_set(&codec_options, "realtime", "true", 0);
        } else if (FFmpegCommonUtils::isMediaCodecEncoder(candidate)) {
            if (output_format_->oformat->flags & AVFMT_GLOBALHEADER) {
                av_dict_set(&codec_options, "ndk_async", "false", 0);
            }
        } else {
            snprintf(crf_buffer, sizeof(crf_buffer), "%d", video_crf_);
            av_dict_set(&codec_options, "preset", video_preset_.c_str(), 0);
            av_dict_set(&codec_options, "crf", crf_buffer, 0);
        }

        const int open_ret = avcodec_open2(image_video_enc_ctx_, candidate, &codec_options);
        av_dict_free(&codec_options);
        return open_ret;
    };

    ret = configureEncoderContext(encoder);
    if (ret < 0) return ret;

    ret = FFmpegCommonUtils::reopenVideoEncoderWithFallback(
        "image video encoder",
        &encoder,
        FFmpegCommonUtils::findSoftwareVideoEncoder(AV_CODEC_ID_H264),
        openEncoder(encoder),
        configureEncoderContext,
        openEncoder
    );
    if (ret < 0) {
        AWESOME_FF_LOGE(
            "Failed to open separate-audio image video encoder: %s",
            FFmpegCommonUtils::codecNameOrUnknown(encoder)
        );
        return ret;
    }

    output_stream = avformat_new_stream(output_format_, nullptr);
    if (!output_stream) return AVERROR(ENOMEM);
    video_output_index_ = output_stream->index;

    ret = avcodec_parameters_from_context(output_stream->codecpar, image_video_enc_ctx_);
    if (ret < 0) return ret;

    output_stream->codecpar->codec_tag = 0;
    output_stream->time_base = image_video_enc_ctx_->time_base;
    output_stream->avg_frame_rate = image_video_enc_ctx_->framerate;
    output_stream->sample_aspect_ratio = image_video_enc_ctx_->sample_aspect_ratio;
    output_stream->disposition = input_stream->disposition;
    av_dict_copy(&output_stream->metadata, input_stream->metadata, 0);

    ret = FFmpegCommonUtils::copyDisplayMatrixSideData(input_stream, output_stream);
    if (ret < 0) return ret;

    image_video_frame_ = av_frame_alloc();
    if (!image_video_frame_) return AVERROR(ENOMEM);

    image_video_frame_->format = image_video_enc_ctx_->pix_fmt;
    image_video_frame_->width = image_video_enc_ctx_->width;
    image_video_frame_->height = image_video_enc_ctx_->height;
    image_video_frame_->sample_aspect_ratio = image_video_enc_ctx_->sample_aspect_ratio;

    ret = av_frame_get_buffer(image_video_frame_, 32);
    if (ret < 0) return ret;

    ret = av_frame_make_writable(image_video_frame_);
    if (ret < 0) return ret;

    sws_ctx_ = sws_getCachedContext(
        sws_ctx_,
        image_source_frame_->width,
        image_source_frame_->height,
        static_cast<AVPixelFormat>(image_source_frame_->format),
        image_video_enc_ctx_->width,
        image_video_enc_ctx_->height,
        image_video_enc_ctx_->pix_fmt,
        SWS_BICUBIC,
        nullptr,
        nullptr,
        nullptr
    );
    if (!sws_ctx_) return AVERROR(ENOMEM);

    const int scaled = sws_scale(
        sws_ctx_,
        image_source_frame_->data,
        image_source_frame_->linesize,
        0,
        image_source_frame_->height,
        image_video_frame_->data,
        image_video_frame_->linesize
    );
    if (scaled <= 0) return AVERROR_EXTERNAL;

    image_video_frame_->pict_type = AV_PICTURE_TYPE_NONE;
    image_video_frame_->color_range = image_source_frame_->color_range;
    image_video_frame_->color_primaries = image_source_frame_->color_primaries;
    image_video_frame_->color_trc = image_source_frame_->color_trc;
    image_video_frame_->colorspace = image_source_frame_->colorspace;
    return 0;
}

int FFmpegMp4SeparateAudioTranscoder::initializeAudioCopyStream() {
    AVStream *input_stream = audio_input_format_->streams[audio_input_index_];
    AVStream *output_stream = avformat_new_stream(output_format_, nullptr);
    if (!output_stream) return AVERROR(ENOMEM);

    int ret = 0;
    if (input_stream->codecpar->codec_id == AV_CODEC_ID_AAC && input_stream->codecpar->extradata_size == 0) {
        ret = audio_bsf_.initialize(input_stream, "aac_adtstoasc");
        if (ret < 0) return ret;

        ret = avcodec_parameters_copy(output_stream->codecpar, audio_bsf_.get()->par_out);
        if (ret < 0) return ret;
        output_stream->time_base = audio_bsf_.get()->time_base_out;
    } else {
        ret = avcodec_parameters_copy(output_stream->codecpar, input_stream->codecpar);
        if (ret < 0) return ret;
        output_stream->time_base = input_stream->time_base;
    }

    audio_output_index_ = output_stream->index;
    output_stream->codecpar->codec_tag = 0;
    output_stream->disposition = input_stream->disposition;
    av_dict_copy(&output_stream->metadata, input_stream->metadata, 0);
    return 0;
}

int FFmpegMp4SeparateAudioTranscoder::initializeAudioTranscodePipeline() {
    AVStream *input_stream = audio_input_format_->streams[audio_input_index_];
    AVStream *output_stream = nullptr;
    AVChannelLayout selected_layout{};
    int ret = 0;

    const AVCodec *encoder = FFmpegCommonUtils::findPreferredAudioEncoder(AV_CODEC_ID_AAC);
    if (!encoder) {
        FFmpegCommonUtils::printMissingAacEncoderHint();
        return AVERROR_ENCODER_NOT_FOUND;
    }
    AWESOME_FF_LOGI("Using separate-audio encoder: %s", FFmpegCommonUtils::codecNameOrUnknown(encoder));

    audio_enc_ctx_ = avcodec_alloc_context3(encoder);
    if (!audio_enc_ctx_) return AVERROR(ENOMEM);

    audio_enc_ctx_->codec_id = encoder->id;
    audio_enc_ctx_->codec_type = AVMEDIA_TYPE_AUDIO;
    audio_enc_ctx_->sample_rate = FFmpegCommonUtils::chooseSampleRate(encoder, audio_dec_ctx_->sample_rate);
    audio_enc_ctx_->sample_fmt = FFmpegCommonUtils::chooseSampleFormat(encoder, audio_dec_ctx_->sample_fmt);
    audio_enc_ctx_->bit_rate = audio_bitrate_;
    audio_enc_ctx_->time_base = AVRational{1, audio_enc_ctx_->sample_rate};

    ret = FFmpegCommonUtils::chooseChannelLayout(encoder, &audio_dec_ctx_->ch_layout, &selected_layout);
    if (ret < 0) goto end;
    ret = av_channel_layout_copy(&audio_enc_ctx_->ch_layout, &selected_layout);
    if (ret < 0) goto end;

    if (encoder->capabilities & AV_CODEC_CAP_EXPERIMENTAL) {
        audio_enc_ctx_->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
    }

    if (output_format_->oformat->flags & AVFMT_GLOBALHEADER) {
        audio_enc_ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    ret = avcodec_open2(audio_enc_ctx_, encoder, nullptr);
    if (ret < 0) {
        AWESOME_FF_LOGE(
            "Failed to open separate-audio AAC encoder: %s",
            FFmpegCommonUtils::codecNameOrUnknown(encoder)
        );
        goto end;
    }

    output_stream = avformat_new_stream(output_format_, nullptr);
    if (!output_stream) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    audio_output_index_ = output_stream->index;

    ret = avcodec_parameters_from_context(output_stream->codecpar, audio_enc_ctx_);
    if (ret < 0) goto end;

    output_stream->codecpar->codec_tag = 0;
    output_stream->time_base = audio_enc_ctx_->time_base;
    output_stream->disposition = input_stream->disposition;
    av_dict_copy(&output_stream->metadata, input_stream->metadata, 0);

    ret = swr_alloc_set_opts2(
        &swr_ctx_,
        &audio_enc_ctx_->ch_layout,
        audio_enc_ctx_->sample_fmt,
        audio_enc_ctx_->sample_rate,
        &audio_dec_ctx_->ch_layout,
        audio_dec_ctx_->sample_fmt,
        audio_dec_ctx_->sample_rate,
        0,
        nullptr
    );
    if (ret < 0) goto end;

    ret = swr_init(swr_ctx_);
    if (ret < 0) goto end;

    audio_fifo_ = av_audio_fifo_alloc(
        audio_enc_ctx_->sample_fmt,
        audio_enc_ctx_->ch_layout.nb_channels,
        1
    );
    if (!audio_fifo_) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    audio_dec_frame_ = av_frame_alloc();
    if (!audio_dec_frame_) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

end:
    av_channel_layout_uninit(&selected_layout);
    return ret;
}

int FFmpegMp4SeparateAudioTranscoder::writeHeader() {
    AVDictionary *muxer_options = nullptr;
    if (faststart_) {
        av_dict_set(&muxer_options, "movflags", "+faststart", 0);
    }

    const int ret = avformat_write_header(output_format_, &muxer_options);
    av_dict_free(&muxer_options);
    return ret;
}

int FFmpegMp4SeparateAudioTranscoder::writeEncoderPackets(AVCodecContext *encoder_context, int output_stream_index) {
    int ret = 0;
    AVStream *output_stream = output_format_->streams[output_stream_index];

    while ((ret = avcodec_receive_packet(encoder_context, enc_packet_)) >= 0) {
        ret = waitIfPaused();
        if (ret < 0) {
            av_packet_unref(enc_packet_);
            return ret;
        }

        av_packet_rescale_ts(enc_packet_, encoder_context->time_base, output_stream->time_base);
        const int64_t timestamp = packetTimestamp(enc_packet_);
        if (isPastTarget(timestamp, output_stream->time_base)) {
            av_packet_unref(enc_packet_);
            continue;
        }
        clampPacketDurationToTarget(enc_packet_, output_stream->time_base);

        enc_packet_->stream_index = output_stream_index;
        enc_packet_->pos = -1;
        progress_.update(timestamp, output_stream->time_base);

        const int write_ret = av_interleaved_write_frame(output_format_, enc_packet_);
        av_packet_unref(enc_packet_);
        if (write_ret < 0) return write_ret;
    }

    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return 0;
    return ret;
}

int FFmpegMp4SeparateAudioTranscoder::processVisualVideoPackets() {
    AVPacket packet = {0};
    int ret = 0;
    AVStream *input_stream = visual_input_format_->streams[visual_input_index_];
    AVStream *output_stream = output_format_->streams[video_output_index_];

    while ((ret = av_read_frame(visual_input_format_, &packet)) >= 0) {
        ret = waitIfPaused();
        if (ret < 0) {
            av_packet_unref(&packet);
            return ret;
        }
        if (isCancelled()) break;

        if (packet.stream_index != visual_input_index_) {
            av_packet_unref(&packet);
            continue;
        }

        packet.pts = normalizePts(packet.pts, input_stream->time_base, output_stream->time_base, visual_start_time_us_);
        packet.dts = normalizePts(packet.dts, input_stream->time_base, output_stream->time_base, visual_start_time_us_);
        if (packet.pts != AV_NOPTS_VALUE && packet.dts != AV_NOPTS_VALUE && packet.dts > packet.pts) {
            packet.dts = packet.pts;
        }
        if (packet.duration > 0) {
            packet.duration = av_rescale_q(packet.duration, input_stream->time_base, output_stream->time_base);
        }

        const int64_t timestamp = packetTimestamp(&packet);
        if (isPastTarget(timestamp, output_stream->time_base)) {
            av_packet_unref(&packet);
            break;
        }
        clampPacketDurationToTarget(&packet, output_stream->time_base);

        packet.stream_index = video_output_index_;
        packet.pos = -1;
        progress_.update(timestamp, output_stream->time_base);

        const int write_ret = av_interleaved_write_frame(output_format_, &packet);
        av_packet_unref(&packet);
        if (write_ret < 0) return write_ret;
    }

    av_packet_unref(&packet);
    if (ret != AVERROR_EOF && ret < 0) return ret;
    return 0;
}

int FFmpegMp4SeparateAudioTranscoder::processStillImageVideo() {
    if (!image_video_enc_ctx_ || !image_video_frame_) return AVERROR(EINVAL);

    const int64_t end_pts = targetDurationPts(image_video_enc_ctx_->time_base);
    if (end_pts == AV_NOPTS_VALUE || end_pts <= 0) return AVERROR(EINVAL);

    for (int64_t pts = 0; pts < end_pts; ++pts) {
        int ret = waitIfPaused();
        if (ret < 0) return ret;
        if (isCancelled()) return 0;

        image_video_frame_->pts = pts;
        image_video_frame_->pict_type = AV_PICTURE_TYPE_NONE;

        ret = avcodec_send_frame(image_video_enc_ctx_, image_video_frame_);
        if (ret < 0) return ret;

        ret = writeEncoderPackets(image_video_enc_ctx_, video_output_index_);
        if (ret < 0) return ret;
    }

    return flushStillImageVideo();
}

int FFmpegMp4SeparateAudioTranscoder::flushStillImageVideo() {
    const int ret = avcodec_send_frame(image_video_enc_ctx_, nullptr);
    if (ret < 0 && ret != AVERROR_EOF) return ret;
    return writeEncoderPackets(image_video_enc_ctx_, video_output_index_);
}

int FFmpegMp4SeparateAudioTranscoder::writeAudioCopyPacket(AVPacket *packet, AVRational source_time_base) {
    if (!packet || audio_output_index_ < 0) return AVERROR(EINVAL);

    int ret = waitIfPaused();
    if (ret < 0) return ret;

    AVStream *output_stream = output_format_->streams[audio_output_index_];

    packet->pts = normalizePts(packet->pts, source_time_base, output_stream->time_base, audio_start_time_us_);
    packet->dts = normalizePts(packet->dts, source_time_base, output_stream->time_base, audio_start_time_us_);
    if (packet->pts != AV_NOPTS_VALUE && packet->dts != AV_NOPTS_VALUE && packet->dts > packet->pts) {
        packet->dts = packet->pts;
    }
    if (packet->duration > 0) {
        packet->duration = av_rescale_q(packet->duration, source_time_base, output_stream->time_base);
    }

    const int64_t timestamp = packetTimestamp(packet);
    if (isPastTarget(timestamp, output_stream->time_base)) {
        return 0;
    }
    clampPacketDurationToTarget(packet, output_stream->time_base);

    packet->stream_index = audio_output_index_;
    packet->pos = -1;
    progress_.update(timestamp, output_stream->time_base);
    return av_interleaved_write_frame(output_format_, packet);
}

int FFmpegMp4SeparateAudioTranscoder::processAudioTranscodePacket(AVPacket *packet) {
    int ret = waitIfPaused();
    if (ret < 0) return ret;

    ret = avcodec_send_packet(audio_dec_ctx_, packet);
    if (ret < 0) return ret;

    while ((ret = avcodec_receive_frame(audio_dec_ctx_, audio_dec_frame_)) >= 0) {
        const int pause_ret = waitIfPaused();
        if (pause_ret < 0) {
            av_frame_unref(audio_dec_frame_);
            return pause_ret;
        }

        if (audio_dec_frame_->pts == AV_NOPTS_VALUE) {
            audio_dec_frame_->pts = audio_dec_frame_->best_effort_timestamp;
        }

        ret = processDecodedAudioFrame(audio_dec_frame_);
        av_frame_unref(audio_dec_frame_);
        if (ret < 0) return ret;
    }

    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return 0;
    return ret;
}

int FFmpegMp4SeparateAudioTranscoder::processDecodedAudioFrame(AVFrame *frame) {
    if (!frame || !audio_enc_ctx_ || !swr_ctx_ || !audio_fifo_) return AVERROR(EINVAL);

    int ret = waitIfPaused();
    if (ret < 0) return ret;

    const int64_t dst_nb_samples_value = av_rescale_rnd(
        swr_get_delay(swr_ctx_, audio_dec_ctx_->sample_rate) + frame->nb_samples,
        audio_enc_ctx_->sample_rate,
        audio_dec_ctx_->sample_rate,
        AV_ROUND_UP
    );
    if (dst_nb_samples_value > INT_MAX) return AVERROR(EINVAL);
    const int dst_nb_samples = static_cast<int>(dst_nb_samples_value);
    if (dst_nb_samples <= 0) return 0;

    AVFrame *converted_frame = av_frame_alloc();
    if (!converted_frame) return AVERROR(ENOMEM);

    converted_frame->nb_samples = dst_nb_samples;
    converted_frame->format = audio_enc_ctx_->sample_fmt;
    converted_frame->sample_rate = audio_enc_ctx_->sample_rate;

    ret = av_channel_layout_copy(&converted_frame->ch_layout, &audio_enc_ctx_->ch_layout);
    if (ret < 0) goto end;

    ret = av_frame_get_buffer(converted_frame, 0);
    if (ret < 0) goto end;

    ret = swr_convert(
        swr_ctx_,
        converted_frame->data,
        dst_nb_samples,
        const_cast<const uint8_t **>(frame->extended_data),
        frame->nb_samples
    );
    if (ret < 0) goto end;

    converted_frame->nb_samples = ret;
    if (converted_frame->nb_samples > 0) {
        ret = av_audio_fifo_realloc(audio_fifo_, av_audio_fifo_size(audio_fifo_) + converted_frame->nb_samples);
        if (ret < 0) goto end;

        ret = av_audio_fifo_write(
            audio_fifo_,
            reinterpret_cast<void **>(converted_frame->data),
            converted_frame->nb_samples
        );
        if (ret < converted_frame->nb_samples) {
            ret = ret < 0 ? ret : AVERROR(EIO);
            goto end;
        }
    }

    if (audio_next_pts_ == AV_NOPTS_VALUE) {
        const int64_t source_pts = frame->pts != AV_NOPTS_VALUE ? frame->pts : frame->best_effort_timestamp;
        audio_next_pts_ = normalizePts(
            source_pts,
            audio_input_format_->streams[audio_input_index_]->time_base,
            audio_enc_ctx_->time_base,
            audio_start_time_us_
        );
        if (audio_next_pts_ == AV_NOPTS_VALUE) audio_next_pts_ = 0;
    }

    ret = drainAudioFifo(0);

end:
    if (converted_frame) av_frame_free(&converted_frame);
    return ret;
}

int FFmpegMp4SeparateAudioTranscoder::drainAudioFifo(int flush_last_frame) {
    if (!audio_enc_ctx_ || !audio_fifo_) return 0;

    const int fixed_frame_size = audio_enc_ctx_->frame_size;
    const int variable_frame_size = audio_enc_ctx_->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE;
    const int64_t end_pts = targetDurationPts(audio_enc_ctx_->time_base);

    while (av_audio_fifo_size(audio_fifo_) > 0) {
        const int pause_ret = waitIfPaused();
        if (pause_ret < 0) return pause_ret;

        if (end_pts != AV_NOPTS_VALUE && audio_next_pts_ != AV_NOPTS_VALUE && audio_next_pts_ >= end_pts) {
            break;
        }

        int target_samples = 0;
        if (!fixed_frame_size || variable_frame_size) {
            target_samples = av_audio_fifo_size(audio_fifo_);
        } else {
            if (!flush_last_frame && av_audio_fifo_size(audio_fifo_) < fixed_frame_size) {
                break;
            }
            target_samples = fixed_frame_size;
        }

        if (end_pts != AV_NOPTS_VALUE) {
            const int64_t remaining_samples = end_pts - (audio_next_pts_ == AV_NOPTS_VALUE ? 0 : audio_next_pts_);
            if (remaining_samples <= 0) break;
            if (remaining_samples < target_samples) {
                target_samples = static_cast<int>(remaining_samples);
            }
        }
        if (target_samples <= 0) break;

        AVFrame *frame = av_frame_alloc();
        if (!frame) return AVERROR(ENOMEM);

        frame->nb_samples = target_samples;
        frame->format = audio_enc_ctx_->sample_fmt;
        frame->sample_rate = audio_enc_ctx_->sample_rate;

        int ret = av_channel_layout_copy(&frame->ch_layout, &audio_enc_ctx_->ch_layout);
        if (ret < 0) {
            av_frame_free(&frame);
            return ret;
        }

        ret = av_frame_get_buffer(frame, 0);
        if (ret < 0) {
            av_frame_free(&frame);
            return ret;
        }

        const int available = av_audio_fifo_size(audio_fifo_);
        const int read_samples = available < target_samples ? available : target_samples;
        ret = av_audio_fifo_read(audio_fifo_, reinterpret_cast<void **>(frame->data), read_samples);
        if (ret < read_samples) {
            av_frame_free(&frame);
            return ret < 0 ? ret : AVERROR(EIO);
        }

        if (read_samples < target_samples) {
            av_samples_set_silence(
                frame->data,
                read_samples,
                target_samples - read_samples,
                audio_enc_ctx_->ch_layout.nb_channels,
                audio_enc_ctx_->sample_fmt
            );
        }

        if (audio_next_pts_ == AV_NOPTS_VALUE) audio_next_pts_ = 0;
        frame->pts = audio_next_pts_;
        audio_next_pts_ += frame->nb_samples;

        ret = avcodec_send_frame(audio_enc_ctx_, frame);
        av_frame_free(&frame);
        if (ret < 0) return ret;

        ret = writeEncoderPackets(audio_enc_ctx_, audio_output_index_);
        if (ret < 0) return ret;
    }

    return 0;
}

int FFmpegMp4SeparateAudioTranscoder::flushResamplerToFifo() {
    if (!swr_ctx_ || !audio_enc_ctx_ || !audio_fifo_) return 0;

    while (swr_get_delay(swr_ctx_, audio_dec_ctx_->sample_rate) > 0) {
        const int pause_ret = waitIfPaused();
        if (pause_ret < 0) return pause_ret;

        const int64_t dst_nb_samples_value = av_rescale_rnd(
            swr_get_delay(swr_ctx_, audio_dec_ctx_->sample_rate),
            audio_enc_ctx_->sample_rate,
            audio_dec_ctx_->sample_rate,
            AV_ROUND_UP
        );
        if (dst_nb_samples_value > INT_MAX) return AVERROR(EINVAL);
        const int dst_nb_samples = static_cast<int>(dst_nb_samples_value);
        if (dst_nb_samples <= 0) break;

        AVFrame *flush_frame = av_frame_alloc();
        if (!flush_frame) return AVERROR(ENOMEM);

        flush_frame->nb_samples = dst_nb_samples;
        flush_frame->format = audio_enc_ctx_->sample_fmt;
        flush_frame->sample_rate = audio_enc_ctx_->sample_rate;

        int ret = av_channel_layout_copy(&flush_frame->ch_layout, &audio_enc_ctx_->ch_layout);
        if (ret < 0) {
            av_frame_free(&flush_frame);
            return ret;
        }

        ret = av_frame_get_buffer(flush_frame, 0);
        if (ret < 0) {
            av_frame_free(&flush_frame);
            return ret;
        }

        ret = swr_convert(swr_ctx_, flush_frame->data, dst_nb_samples, nullptr, 0);
        if (ret < 0) {
            av_frame_free(&flush_frame);
            return ret;
        }
        if (ret == 0) {
            av_frame_free(&flush_frame);
            break;
        }

        flush_frame->nb_samples = ret;
        ret = av_audio_fifo_realloc(audio_fifo_, av_audio_fifo_size(audio_fifo_) + flush_frame->nb_samples);
        if (ret < 0) {
            av_frame_free(&flush_frame);
            return ret;
        }

        ret = av_audio_fifo_write(
            audio_fifo_,
            reinterpret_cast<void **>(flush_frame->data),
            flush_frame->nb_samples
        );
        av_frame_free(&flush_frame);
        if (ret < 0) return ret;
    }

    return 0;
}

int FFmpegMp4SeparateAudioTranscoder::flushAudioTranscodePipeline() {
    if (!audio_dec_ctx_ || !audio_enc_ctx_) return 0;

    int ret = waitIfPaused();
    if (ret < 0) return ret;

    ret = avcodec_send_packet(audio_dec_ctx_, nullptr);
    if (ret < 0 && ret != AVERROR_EOF) return ret;

    while ((ret = avcodec_receive_frame(audio_dec_ctx_, audio_dec_frame_)) >= 0) {
        const int pause_ret = waitIfPaused();
        if (pause_ret < 0) {
            av_frame_unref(audio_dec_frame_);
            return pause_ret;
        }

        if (audio_dec_frame_->pts == AV_NOPTS_VALUE) {
            audio_dec_frame_->pts = audio_dec_frame_->best_effort_timestamp;
        }

        ret = processDecodedAudioFrame(audio_dec_frame_);
        av_frame_unref(audio_dec_frame_);
        if (ret < 0) return ret;
    }
    if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) return ret;

    ret = flushResamplerToFifo();
    if (ret < 0) return ret;

    ret = drainAudioFifo(1);
    if (ret < 0) return ret;

    ret = avcodec_send_frame(audio_enc_ctx_, nullptr);
    if (ret < 0 && ret != AVERROR_EOF) return ret;

    return writeEncoderPackets(audio_enc_ctx_, audio_output_index_);
}

int FFmpegMp4SeparateAudioTranscoder::processAudioPackets() {
    AVPacket packet = {0};
    int ret = 0;
    AVStream *input_stream = audio_input_format_->streams[audio_input_index_];

    while ((ret = av_read_frame(audio_input_format_, &packet)) >= 0) {
        ret = waitIfPaused();
        if (ret < 0) {
            av_packet_unref(&packet);
            return ret;
        }
        if (isCancelled()) break;

        if (packet.stream_index != audio_input_index_) {
            av_packet_unref(&packet);
            continue;
        }

        const int64_t input_timestamp = normalizePts(
            packetTimestamp(&packet),
            input_stream->time_base,
            input_stream->time_base,
            audio_start_time_us_
        );
        if (isPastTarget(input_timestamp, input_stream->time_base)) {
            av_packet_unref(&packet);
            break;
        }

        if (audio_copy_mode_) {
            if (audio_bsf_.get()) {
                ret = av_bsf_send_packet(audio_bsf_.get(), &packet);
                av_packet_unref(&packet);
                if (ret < 0) return ret;

                while ((ret = av_bsf_receive_packet(audio_bsf_.get(), &packet)) == 0) {
                    const int write_ret = writeAudioCopyPacket(&packet, audio_bsf_.get()->time_base_out);
                    av_packet_unref(&packet);
                    if (write_ret < 0) return write_ret;
                }

                if (ret == AVERROR(EAGAIN)) continue;
                if (ret == AVERROR_EOF) break;
                if (ret < 0) return ret;
            } else {
                ret = writeAudioCopyPacket(&packet, input_stream->time_base);
                av_packet_unref(&packet);
                if (ret < 0) return ret;
            }
        } else {
            ret = processAudioTranscodePacket(&packet);
            av_packet_unref(&packet);
            if (ret < 0) return ret;
        }
    }

    av_packet_unref(&packet);
    if (ret != AVERROR_EOF && ret < 0) return ret;
    if (isCancelled()) return 0;

    if (audio_copy_mode_) {
        if (audio_bsf_.get()) {
            ret = waitIfPaused();
            if (ret < 0) return ret;

            ret = av_bsf_send_packet(audio_bsf_.get(), nullptr);
            if (ret < 0 && ret != AVERROR_EOF) return ret;

            while ((ret = av_bsf_receive_packet(audio_bsf_.get(), &packet)) == 0) {
                const int pause_ret = waitIfPaused();
                if (pause_ret < 0) {
                    av_packet_unref(&packet);
                    return pause_ret;
                }
                const int write_ret = writeAudioCopyPacket(&packet, audio_bsf_.get()->time_base_out);
                av_packet_unref(&packet);
                if (write_ret < 0) return write_ret;
            }

            if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) return ret;
        }
        return 0;
    }

    return flushAudioTranscodePipeline();
}

int FFmpegMp4SeparateAudioTranscoder::transcode() {
    if (visual_input_path_.empty() || audio_input_path_.empty() || output_path_.empty()) {
        return AVERROR(EINVAL);
    }
    if (visual_input_path_ == output_path_ || audio_input_path_ == output_path_) {
        return AVERROR(EINVAL);
    }

    int ret = openInputs();
    if (ret < 0) {
        FFmpegCommonUtils::printError("Failed to open visual/audio inputs", ret);
        return ret;
    }

    ret = openOutput();
    if (ret < 0) {
        FFmpegCommonUtils::printError("Failed to prepare separate-audio output", ret);
        return ret;
    }

    ret = waitIfPaused();
    if (ret < 0) return ret;

    ret = writeHeader();
    if (ret < 0) {
        FFmpegCommonUtils::printError("Failed to write separate-audio mp4 header", ret);
        return ret;
    }

    ret = waitIfPaused();
    if (ret < 0) return ret;

    ret = visual_is_image_ ? processStillImageVideo() : processVisualVideoPackets();
    if (ret < 0) {
        FFmpegCommonUtils::printError("Failed to process visual input", ret);
        return ret;
    }

    ret = waitIfPaused();
    if (ret < 0) return ret;

    ret = processAudioPackets();
    if (ret < 0) {
        FFmpegCommonUtils::printError("Failed to process separate audio input", ret);
        return ret;
    }

    if (isCancelled()) return 0;

    ret = av_write_trailer(output_format_);
    if (ret < 0) {
        FFmpegCommonUtils::printError("Failed to write separate-audio mp4 trailer", ret);
        return ret;
    }

    progress_.finish();
    return 0;
}

} // namespace

int transcode_file_with_separate_audio_to_mp4_cpp(
    const char *visual_input_path,
    const char *audio_input_path,
    const char *output_path,
    const FFmpegMp4TranscodeConfig *config,
    void (*progress_cb)(int percentage, void *user_data),
    void *user_data,
    volatile int *cancel_flag,
    volatile int *pause_flag
) {
    FFmpegMp4SeparateAudioTranscoder transcoder(
        visual_input_path,
        audio_input_path,
        output_path,
        config,
        progress_cb,
        user_data,
        cancel_flag,
        pause_flag
    );
    return transcoder.transcode();
}

int extract_audio_stream_from_media_cpp(
    const char *input_path,
    const char *output_path,
    void (*progress_cb)(int percentage, void *user_data),
    void *user_data,
    volatile int *cancel_flag,
    volatile int *pause_flag
) {
    FFmpegAudioExtractor extractor(
        input_path,
        output_path,
        progress_cb,
        user_data,
        cancel_flag,
        pause_flag
    );
    return extractor.extract();
}
