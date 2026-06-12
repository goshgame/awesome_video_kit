#include "ffmpeg_mp4_concat_image_transcoder.h"

#include "ffmpeg_common_utils.h"
#include "ffmpeg_mp4_concat_image_support.h"

FFmpegMp4ConcatImageTranscoder::FFmpegMp4ConcatImageTranscoder(
    const char *input_path,
    const char *output_path,
    const FFmpegMp4ConcatImageConfig *concat_image,
    void (*progress_cb)(int percentage, void *user_data),
    void *user_data,
    volatile int *cancel_flag,
    volatile int *pause_flag
) : FFmpegMp4TranscoderBase(
        input_path,
        output_path,
        progress_cb,
        user_data,
        cancel_flag,
        pause_flag
    ),
    concat_image_path_(concat_image && concat_image->image_path ? concat_image->image_path : ""),
    concat_image_duration_us_(concat_image && concat_image->image_duration_us > 0 ? concat_image->image_duration_us : 0),
    concat_image_position_(concat_image ? concat_image->position : FFmpegConcatImagePositionTail),
    video_enc_frame_(nullptr),
    concat_image_frame_(nullptr),
    last_video_frame_(nullptr),
    input_duration_us_(0),
    video_next_pts_(0),
    video_segment_offset_pts_(0) {
    video_crf_ = 18;
}

FFmpegMp4ConcatImageTranscoder::~FFmpegMp4ConcatImageTranscoder() {
    cleanup();
}

void FFmpegMp4ConcatImageTranscoder::cleanup() {
    if (last_video_frame_) av_frame_free(&last_video_frame_);
    if (concat_image_frame_) av_frame_free(&concat_image_frame_);
    if (video_enc_frame_) av_frame_free(&video_enc_frame_);
    cleanupSharedResources();
    input_duration_us_ = 0;
    video_next_pts_ = 0;
    video_segment_offset_pts_ = 0;
}

bool FFmpegMp4ConcatImageTranscoder::hasConcatImage() const {
    return !concat_image_path_.empty() && concat_image_duration_us_ > 0;
}

int FFmpegMp4ConcatImageTranscoder::openInput() {
    const int ret = openInputFileAndInitDecoders();
    if (ret < 0) return ret;

    input_duration_us_ = FFmpegCommonUtils::getDurationInUsSafe(input_format_);
    const int64_t progress_duration_us = input_duration_us_ > 0
        ? input_duration_us_ + concat_image_duration_us_
        : 0;
    progress_.reset(progress_duration_us, progress_cb_, user_data_);
    return 0;
}

int FFmpegMp4ConcatImageTranscoder::openOutput() {
    int ret = avformat_alloc_output_context2(&output_format_, nullptr, "mp4", output_path_.c_str());
    if (ret < 0) return ret;

    ret = initializeVideoEncoder();
    if (ret < 0) {
        FFmpegCommonUtils::printError("Failed to initialize concat image video encoder", ret);
        return ret;
    }

    if (audio_dec_ctx_) {
        ret = initializeAudioEncoder();
        if (ret < 0) {
            FFmpegCommonUtils::printError("Failed to initialize concat image audio encoder", ret);
            return ret;
        }
    }

    ret = initializeFrames();
    if (ret < 0) return ret;

    ret = initializeConcatImageFrame();
    if (ret < 0) return ret;

    if (!(output_format_->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&output_format_->pb, output_path_.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) return ret;
    }

    av_dict_copy(&output_format_->metadata, input_format_->metadata, 0);
    return 0;
}

int FFmpegMp4ConcatImageTranscoder::initializeVideoEncoder() {
    return configureVideoEncoder(true);
}

int FFmpegMp4ConcatImageTranscoder::configureVideoEncoder(bool create_output_stream) {
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

        const bool needs_global_header = (output_format_->oformat->flags & AVFMT_GLOBALHEADER) != 0;
        if (needs_global_header && !FFmpegCommonUtils::isMediaCodecEncoder(candidate)) {
            video_enc_ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }

        AWESOME_FF_LOGI(
            "Configured concat image video encoder context: candidate=%s size=%dx%d pix_fmt=%s time_base=%d/%d frame_rate=%d/%d global_header=%d",
            FFmpegCommonUtils::codecNameOrUnknown(candidate),
            video_enc_ctx_->width,
            video_enc_ctx_->height,
            av_get_pix_fmt_name(video_enc_ctx_->pix_fmt) ? av_get_pix_fmt_name(video_enc_ctx_->pix_fmt) : "unknown",
            video_enc_ctx_->time_base.num,
            video_enc_ctx_->time_base.den,
            video_enc_ctx_->framerate.num,
            video_enc_ctx_->framerate.den,
            (video_enc_ctx_->flags & AV_CODEC_FLAG_GLOBAL_HEADER) ? 1 : 0
        );

        FFmpegCommonUtils::sanitizeVideoEncoderContextDefaults(video_enc_ctx_, candidate);
        return 0;
    };

    auto openEncoder = [&](const AVCodec *candidate) -> int {
        AVDictionary *codec_options = nullptr;
        char crf_buffer[16];

        av_dict_set(&codec_options, "profile", video_profile_.c_str(), 0);
        av_dict_set(&codec_options, "level", video_level_.c_str(), 0);
        if (FFmpegCommonUtils::isVideoToolboxEncoder(candidate)) {
            av_dict_set(&codec_options, "realtime", "true", 0);
            av_dict_set(&codec_options, "forced-idr", "true", 0);
            if (!create_output_stream && video_packets_written_) {
                av_dict_set(&codec_options, "frames_before", "true", 0);
            }
        } else if (FFmpegCommonUtils::isMediaCodecEncoder(candidate)) {
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
            "Requested concat output video size: %dx%d, actual encoder size: %dx%d",
            scale_width_ > 0 ? scale_width_ : 0,
            scale_height_ > 0 ? scale_height_ : 0,
            video_enc_ctx_->width,
            video_enc_ctx_->height
        );
    }

    if (create_output_stream) {
        ret = FFmpegCommonUtils::reopenVideoEncoderWithFallback(
            "concat image video encoder",
            &encoder,
            FFmpegCommonUtils::findSoftwareVideoEncoder(AV_CODEC_ID_H264),
            openEncoder(encoder),
            configureEncoderContext,
            openEncoder
        );
        if (ret < 0) {
            AWESOME_FF_LOGE(
                "Failed to open concat image video encoder: %s",
                FFmpegCommonUtils::codecNameOrUnknown(encoder)
            );
            return ret;
        }
    } else {
        ret = openEncoder(encoder);
        if (ret < 0) {
            AWESOME_FF_LOGE(
                "Failed to reopen concat runtime video encoder without software fallback: %s",
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

int FFmpegMp4ConcatImageTranscoder::initializeAudioEncoder() {
    AVStream *input_stream = input_format_->streams[audio_input_index_];
    AVStream *output_stream = nullptr;
    AVChannelLayout selected_layout{};
    int ret = 0;

    const AVCodec *encoder = FFmpegCommonUtils::findPreferredAudioEncoder(AV_CODEC_ID_AAC);
    if (!encoder) {
        FFmpegCommonUtils::printMissingAacEncoderHint();
        return AVERROR_ENCODER_NOT_FOUND;
    }
    AWESOME_FF_LOGI("Using concat image audio encoder: %s", FFmpegCommonUtils::codecNameOrUnknown(encoder));

    audio_enc_ctx_ = avcodec_alloc_context3(encoder);
    if (!audio_enc_ctx_) return AVERROR(ENOMEM);

    audio_enc_ctx_->codec_id = encoder->id;
    audio_enc_ctx_->codec_type = AVMEDIA_TYPE_AUDIO;
    audio_enc_ctx_->sample_rate = FFmpegCommonUtils::chooseSampleRate(encoder, audio_dec_ctx_->sample_rate);
    audio_enc_ctx_->sample_fmt = FFmpegCommonUtils::chooseSampleFormat(encoder, audio_dec_ctx_->sample_fmt);
    audio_enc_ctx_->bit_rate = input_stream->codecpar->bit_rate > 0
        ? input_stream->codecpar->bit_rate
        : (audio_bitrate_ > 0 ? audio_bitrate_ : 128000);
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
            "Failed to open concat image audio encoder: %s",
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

int FFmpegMp4ConcatImageTranscoder::initializeFrames() {
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

int FFmpegMp4ConcatImageTranscoder::refreshVideoEncodeBuffer() {
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

int FFmpegMp4ConcatImageTranscoder::rebuildVideoEncoderForResume() {
    const int ret = configureVideoEncoder(false);
    if (ret < 0) return ret;

    if (last_video_frame_) av_frame_free(&last_video_frame_);
    if (concat_image_frame_) av_frame_free(&concat_image_frame_);
    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }

    int refresh_ret = refreshVideoEncodeBuffer();
    if (refresh_ret < 0) return refresh_ret;

    if (hasConcatImage()) {
        refresh_ret = initializeConcatImageFrame();
        if (refresh_ret < 0) return refresh_ret;
    }

    return 0;
}

int FFmpegMp4ConcatImageTranscoder::initializeConcatImageFrame() {
    return FFmpegMp4ConcatImageSupport::initializeConcatImageFrame(
        concat_image_path_.c_str(),
        input_format_,
        video_input_index_,
        video_enc_ctx_,
        &sws_ctx_,
        "concat image",
        &concat_image_frame_
    );
}

int FFmpegMp4ConcatImageTranscoder::sendPreparedVideoFrame(AVFrame *frame, int64_t output_pts) {
    if (!frame) return AVERROR(EINVAL);

    if (output_pts == AV_NOPTS_VALUE || output_pts < video_next_pts_) {
        output_pts = video_next_pts_;
    }

    frame->pict_type = AV_PICTURE_TYPE_NONE;
    if (frame->sample_aspect_ratio.num <= 0 || frame->sample_aspect_ratio.den <= 0) {
        frame->sample_aspect_ratio = video_enc_ctx_->sample_aspect_ratio;
    }

    if (output_pts != AV_NOPTS_VALUE) {
        progress_.update(output_pts, video_enc_ctx_->time_base);
    }

    const int ret = sendVideoFrameWithRecovery(frame, output_pts, "Concat image video frame encode");
    if (ret < 0) return ret;

    if (output_pts != AV_NOPTS_VALUE && output_pts + 1 > video_next_pts_) {
        video_next_pts_ = output_pts + 1;
    }

    return cacheFrameReference(&last_video_frame_, frame);
}

int FFmpegMp4ConcatImageTranscoder::duplicateLastVideoFrameUntil(int64_t target_pts) {
    if (!hasForcedVideoFrameRate() || target_pts == AV_NOPTS_VALUE || !last_video_frame_) {
        return 0;
    }

    while (video_next_pts_ < target_pts) {
        int ret = waitIfPaused();
        if (ret < 0) return ret;
        ret = rebuildVideoEncoderAfterPauseIfNeeded("Rebuilding hardware video encoder after pause/resume.");
        if (ret < 0) return ret;
        if (!last_video_frame_) return 0;

        AVFrame *duplicate_frame = av_frame_clone(last_video_frame_);
        if (!duplicate_frame) return AVERROR(ENOMEM);

        ret = sendPreparedVideoFrame(duplicate_frame, video_next_pts_);
        av_frame_free(&duplicate_frame);
        if (ret < 0) return ret;
    }

    return 0;
}

int FFmpegMp4ConcatImageTranscoder::processDecodedVideoFrame(AVFrame *frame) {
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
    int64_t output_pts = normalizePts(
        source_pts,
        input_format_->streams[video_input_index_]->time_base,
        video_enc_ctx_->time_base,
        video_segment_offset_pts_
    );
    if (output_pts == AV_NOPTS_VALUE) {
        output_pts = video_next_pts_;
    }
    if (hasForcedVideoFrameRate()) {
        if (!last_video_frame_ && output_pts > video_next_pts_) {
            output_pts = video_next_pts_;
        }

        ret = duplicateLastVideoFrameUntil(output_pts);
        if (ret < 0) return ret;

        if (output_pts < video_next_pts_) {
            return 0;
        }
    }

    video_enc_frame_->sample_aspect_ratio = frame->sample_aspect_ratio.num > 0 && frame->sample_aspect_ratio.den > 0
        ? frame->sample_aspect_ratio
        : video_enc_ctx_->sample_aspect_ratio;
    video_enc_frame_->color_range = frame->color_range;
    video_enc_frame_->color_primaries = frame->color_primaries;
    video_enc_frame_->color_trc = frame->color_trc;
    video_enc_frame_->colorspace = frame->colorspace;

    return sendPreparedVideoFrame(video_enc_frame_, output_pts);
}

int FFmpegMp4ConcatImageTranscoder::processVideoPacket(AVPacket *packet) {
    int ret = normalizeInputVideoPacketForDecoder(packet);
    if (ret < 0) return ret;

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

int FFmpegMp4ConcatImageTranscoder::processDecodedAudioFrame(AVFrame *frame) {
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

int FFmpegMp4ConcatImageTranscoder::processAudioPacket(AVPacket *packet) {
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

int FFmpegMp4ConcatImageTranscoder::drainAudioFifo(int flush_last_frame) {
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

int FFmpegMp4ConcatImageTranscoder::flushResamplerToFifo() {
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

int FFmpegMp4ConcatImageTranscoder::flushSourceVideoDecoder() {
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

    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return 0;
    return ret;
}

int FFmpegMp4ConcatImageTranscoder::flushSourceAudioDecoder() {
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

    return drainAudioFifo(0);
}

int FFmpegMp4ConcatImageTranscoder::finalizeVideoEncoder() {
    int ret = avcodec_send_frame(video_enc_ctx_, nullptr);
    if (ret < 0 && ret != AVERROR_EOF) return ret;
    return writeEncodedPackets(video_enc_ctx_, video_output_index_);
}

int FFmpegMp4ConcatImageTranscoder::finalizeAudioEncoder() {
    if (!audio_enc_ctx_) return 0;

    int ret = drainAudioFifo(1);
    if (ret < 0) return ret;

    ret = avcodec_send_frame(audio_enc_ctx_, nullptr);
    if (ret < 0 && ret != AVERROR_EOF) return ret;

    return writeEncodedPackets(audio_enc_ctx_, audio_output_index_);
}

int FFmpegMp4ConcatImageTranscoder::generateConcatImageVideoSegment(int64_t start_time_us) {
    FFmpegMp4ConcatImageRuntimeCallbacks callbacks;
    callbacks.is_cancelled = [this]() { return isCancelled(); };
    callbacks.wait_if_paused = [this]() { return waitIfPaused(); };
    callbacks.rebuild_video_encoder_after_pause = [this](const char *reason) {
        return rebuildVideoEncoderAfterPauseIfNeeded(reason);
    };
    callbacks.send_prepared_video_frame = [this](AVFrame *frame, int64_t output_pts) {
        return sendPreparedVideoFrame(frame, output_pts);
    };
    return FFmpegMp4ConcatImageSupport::generateConcatImageVideoSegment(
        &video_enc_ctx_,
        &video_enc_frame_,
        &concat_image_frame_,
        concat_image_duration_us_,
        start_time_us,
        &video_next_pts_,
        callbacks
    );
}

int FFmpegMp4ConcatImageTranscoder::appendSilentAudioSegment(int64_t duration_us) {
    FFmpegMp4ConcatImageRuntimeCallbacks callbacks;
    callbacks.wait_if_paused = [this]() { return waitIfPaused(); };
    callbacks.drain_audio_fifo = [this](int flush_last_frame) {
        return drainAudioFifo(flush_last_frame);
    };
    return FFmpegMp4ConcatImageSupport::appendSilentAudioSegment(
        audio_enc_ctx_,
        audio_fifo_,
        duration_us,
        &audio_next_pts_,
        callbacks
    );
}

int FFmpegMp4ConcatImageTranscoder::prependConcatImageSegmentIfNeeded() {
    if (!hasConcatImage() || concat_image_position_ != FFmpegConcatImagePositionHead) {
        return 0;
    }

    AWESOME_FF_LOGI(
        "Prepending concat image segment: duration_us=%lld",
        static_cast<long long>(concat_image_duration_us_)
    );

    int ret = generateConcatImageVideoSegment(0);
    if (ret < 0) return ret;

    video_segment_offset_pts_ = video_next_pts_;

    if (audio_enc_ctx_) {
        ret = appendSilentAudioSegment(concat_image_duration_us_);
        if (ret < 0) return ret;
    }

    return 0;
}

int FFmpegMp4ConcatImageTranscoder::appendConcatImageSegmentIfNeeded() {
    if (!hasConcatImage() || concat_image_position_ != FFmpegConcatImagePositionTail) {
        return 0;
    }

    int64_t image_start_us = currentVideoOutputTimeUs();
    const int64_t audio_time_us = currentAudioOutputTimeUs();
    if (audio_time_us > image_start_us) {
        image_start_us = audio_time_us;
    }

    AWESOME_FF_LOGI(
        "Appending concat image segment: video_time_us=%lld audio_time_us=%lld start_us=%lld duration_us=%lld",
        static_cast<long long>(currentVideoOutputTimeUs()),
        static_cast<long long>(audio_time_us),
        static_cast<long long>(image_start_us),
        static_cast<long long>(concat_image_duration_us_)
    );

    int ret = generateConcatImageVideoSegment(image_start_us);
    if (ret < 0) return ret;

    if (audio_enc_ctx_) {
        ret = appendSilentAudioSegment(concat_image_duration_us_);
        if (ret < 0) return ret;
    }

    return 0;
}

int64_t FFmpegMp4ConcatImageTranscoder::currentVideoOutputTimeUs() const {
    return FFmpegMp4ConcatImageSupport::currentVideoOutputTimeUs(video_enc_ctx_, video_next_pts_);
}

int64_t FFmpegMp4ConcatImageTranscoder::currentAudioOutputTimeUs() const {
    return FFmpegMp4ConcatImageSupport::currentAudioOutputTimeUs(audio_enc_ctx_, audio_fifo_, audio_next_pts_);
}

int FFmpegMp4ConcatImageTranscoder::transcodeLoop() {
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
            ret = processAudioPacket(&packet);
        }

        av_packet_unref(&packet);
        if (ret < 0) return ret;
    }

    av_packet_unref(&packet);
    if (ret != AVERROR_EOF && ret < 0) return ret;
    return 0;
}

int FFmpegMp4ConcatImageTranscoder::transcode() {
    int ret = 0;

    if (input_path_.empty() || output_path_.empty()) return AVERROR(EINVAL);
    if (input_path_ == output_path_) return AVERROR(EINVAL);
    if (!hasConcatImage()) return AVERROR(EINVAL);

    ret = openInput();
    if (ret < 0) {
        FFmpegCommonUtils::printError("Failed to open concat image input", ret);
        return ret;
    }

    ret = openOutput();
    if (ret < 0) {
        FFmpegCommonUtils::printError("Failed to prepare concat image output", ret);
        return ret;
    }

    ret = writeHeader();
    if (ret < 0) {
        FFmpegCommonUtils::printError("Failed to write concat image mp4 header", ret);
        return ret;
    }

    ret = prependConcatImageSegmentIfNeeded();
    if (ret < 0) {
        FFmpegCommonUtils::printError("Failed to prepend concat image segment", ret);
        return ret;
    }

    ret = transcodeLoop();
    if (ret < 0) {
        FFmpegCommonUtils::printError("Concat image transcode loop failed", ret);
        return ret;
    }
    if (isCancelled()) return 0;

    ret = flushSourceVideoDecoder();
    if (ret < 0) {
        FFmpegCommonUtils::printError("Failed to flush concat image video decoder", ret);
        return ret;
    }

    ret = flushSourceAudioDecoder();
    if (ret < 0) {
        FFmpegCommonUtils::printError("Failed to flush concat image audio decoder", ret);
        return ret;
    }

    ret = appendConcatImageSegmentIfNeeded();
    if (ret < 0) {
        FFmpegCommonUtils::printError("Failed to append concat image segment", ret);
        return ret;
    }

    ret = finalizeVideoEncoder();
    if (ret < 0) {
        FFmpegCommonUtils::printError("Failed to finalize concat image video encoder", ret);
        return ret;
    }

    ret = finalizeAudioEncoder();
    if (ret < 0) {
        FFmpegCommonUtils::printError("Failed to finalize concat image audio encoder", ret);
        return ret;
    }

    ret = av_write_trailer(output_format_);
    if (ret < 0) {
        FFmpegCommonUtils::printError("Failed to write concat image trailer", ret);
        return ret;
    }

    logOutputFileProbeSummary();

    progress_.finish();
    return 0;
}
