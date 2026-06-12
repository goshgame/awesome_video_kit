//
//  ffmpeg_hls_watermark_pipeline.cpp
//  AwesomeVideoKitSDK
//
//  Created by dev on 2026/3/20.
//

#include "ffmpeg_hls_watermark_pipeline.h"
#include "ffmpeg_common_utils.h"
#include "ffmpeg_watermark_filter_utils.h"

namespace {

int64_t packetTimestampAnchor(const AVPacket *packet) {
    if (!packet) return 0;
    if (packet->dts != AV_NOPTS_VALUE) return packet->dts;
    if (packet->pts != AV_NOPTS_VALUE) return packet->pts;
    return 0;
}

void shiftPacketTimestamps(AVPacket *packet, int64_t shift) {
    if (!packet || shift == 0) return;
    if (packet->pts != AV_NOPTS_VALUE) packet->pts += shift;
    if (packet->dts != AV_NOPTS_VALUE) packet->dts += shift;
}

int64_t muxerNextVideoDts(AVStream *stream) {
    if (!stream) return AV_NOPTS_VALUE;
#if FF_API_GET_END_PTS
    return av_stream_get_end_pts(stream);
#else
    return AV_NOPTS_VALUE;
#endif
}

int64_t minimumNextVideoDts(AVStream *stream, int64_t last_written_dts) {
    int64_t minimum_dts = AV_NOPTS_VALUE;
    if (last_written_dts != AV_NOPTS_VALUE) {
        minimum_dts = last_written_dts + 1;
    }

    const int64_t muxer_next_dts = muxerNextVideoDts(stream);
    if (muxer_next_dts != AV_NOPTS_VALUE &&
        (minimum_dts == AV_NOPTS_VALUE || muxer_next_dts > minimum_dts)) {
        minimum_dts = muxer_next_dts;
    }
    return minimum_dts;
}

void prepareVideoFrameForEncoding(
    AVFrame *frame,
    int64_t absolute_pts,
    int64_t *origin_pts,
    bool *force_keyframe
) {
    if (!frame || !origin_pts || !force_keyframe) return;

    if (absolute_pts == AV_NOPTS_VALUE) {
        absolute_pts = 0;
    }
    if (*origin_pts == AV_NOPTS_VALUE) {
        *origin_pts = absolute_pts;
    }

    frame->pts = absolute_pts - *origin_pts;
    if (frame->pts < 0) frame->pts = 0;

    if (*force_keyframe) {
        frame->pict_type = AV_PICTURE_TYPE_I;
        frame->flags |= AV_FRAME_FLAG_KEY;
        *force_keyframe = false;
    } else {
        frame->pict_type = AV_PICTURE_TYPE_NONE;
        frame->flags &= ~AV_FRAME_FLAG_KEY;
    }
}

} // namespace

FFmpegWatermarkPipeline::FFmpegWatermarkPipeline()
#if AWESOME_HAS_AVFILTER
    : dec_ctx_(nullptr),
      enc_ctx_(nullptr),
      filter_graph_(nullptr),
      buffer_src_ctx_(nullptr),
      buffer_sink_ctx_(nullptr),
      dec_frame_(nullptr),
      filtered_frame_(nullptr),
      enc_packet_(nullptr),
      input_stream_index_(-1),
      output_stream_index_(-1),
      filter_time_base_(AVRational{0, 1}),
      input_format_(nullptr),
      output_format_(nullptr),
      watermark_(nullptr),
      frame_rate_(AVRational{0, 1}),
      video_encoder_input_origin_pts_(AV_NOPTS_VALUE),
      video_encoder_output_offset_pts_(AV_NOPTS_VALUE),
      video_last_written_pts_(AV_NOPTS_VALUE),
      video_last_written_dts_(AV_NOPTS_VALUE),
      video_last_write_packet_count_(0),
      video_force_keyframe_on_next_frame_(false),
      video_packets_written_(false),
      video_muxer_reopen_required_(false),
      video_extradata_snapshot_()
#endif
{
}

FFmpegWatermarkPipeline::~FFmpegWatermarkPipeline() {
#if AWESOME_HAS_AVFILTER
    cleanup();
#endif
}

int FFmpegWatermarkPipeline::isEnabled(const FFmpegWatermarkConfig *watermark) {
    return FFmpegWatermarkFilterUtils::isEnabled(watermark);
}

#if !AWESOME_HAS_AVFILTER
int FFmpegWatermarkPipeline::initialize(
    AVFormatContext *,
    AVFormatContext *,
    int,
    int,
    const FFmpegWatermarkConfig *
) {
    return AVERROR(ENOSYS);
}

int FFmpegWatermarkPipeline::processPacket(
    AVPacket *,
    AVFormatContext *,
    FFmpegProgressTracker *
) {
    return AVERROR(ENOSYS);
}

int FFmpegWatermarkPipeline::flush(AVFormatContext *, FFmpegProgressTracker *) {
    return AVERROR(ENOSYS);
}
#else
void FFmpegWatermarkPipeline::cleanup() {
    if (enc_packet_) av_packet_free(&enc_packet_);
    if (filtered_frame_) av_frame_free(&filtered_frame_);
    if (dec_frame_) av_frame_free(&dec_frame_);
    if (filter_graph_) avfilter_graph_free(&filter_graph_);
    if (enc_ctx_) avcodec_free_context(&enc_ctx_);
    if (dec_ctx_) avcodec_free_context(&dec_ctx_);

    buffer_src_ctx_ = nullptr;
    buffer_sink_ctx_ = nullptr;
    input_stream_index_ = -1;
    output_stream_index_ = -1;
    filter_time_base_ = AVRational{0, 1};
    input_format_ = nullptr;
    output_format_ = nullptr;
    watermark_ = nullptr;
    frame_rate_ = AVRational{0, 1};
    video_encoder_input_origin_pts_ = AV_NOPTS_VALUE;
    video_encoder_output_offset_pts_ = AV_NOPTS_VALUE;
    video_last_written_pts_ = AV_NOPTS_VALUE;
    video_last_written_dts_ = AV_NOPTS_VALUE;
    video_last_write_packet_count_ = 0;
    video_force_keyframe_on_next_frame_ = false;
    video_packets_written_ = false;
    video_muxer_reopen_required_ = false;
    video_extradata_snapshot_.clear();
}

int FFmpegWatermarkPipeline::initializeFilterGraph(
    AVStream *video_input_stream,
    const FFmpegWatermarkConfig *watermark
) {
    return FFmpegWatermarkFilterUtils::initializeWatermarkFilterGraph(
        dec_ctx_,
        enc_ctx_,
        video_input_stream,
        dec_ctx_->width,
        dec_ctx_->height,
        watermark,
        &filter_graph_,
        &buffer_src_ctx_,
        &buffer_sink_ctx_,
        &filter_time_base_
    );
}

int FFmpegWatermarkPipeline::configureEncoder() {
    int ret = 0;
    AVStream *video_input_stream = input_format_->streams[input_stream_index_];
    AVStream *video_output_stream = output_format_->streams[output_stream_index_];
    const AVCodec *encoder = nullptr;
    const enum AVCodecID preferred_codec_id = video_input_stream->codecpar->codec_id;

    encoder = FFmpegCommonUtils::findFallbackVideoEncoder(preferred_codec_id);
    if (!encoder) return AVERROR_ENCODER_NOT_FOUND;

    if (frame_rate_.num <= 0 || frame_rate_.den <= 0) frame_rate_ = AVRational{25, 1};

    auto configureEncoderContext = [&](const AVCodec *candidate) -> int {
        if (enc_ctx_) avcodec_free_context(&enc_ctx_);

        enc_ctx_ = avcodec_alloc_context3(candidate);
        if (!enc_ctx_) return AVERROR(ENOMEM);

        enc_ctx_->width = dec_ctx_->width;
        enc_ctx_->height = dec_ctx_->height;
        enc_ctx_->sample_aspect_ratio = dec_ctx_->sample_aspect_ratio;
        if (enc_ctx_->sample_aspect_ratio.num <= 0 || enc_ctx_->sample_aspect_ratio.den <= 0) {
            enc_ctx_->sample_aspect_ratio = video_input_stream->sample_aspect_ratio;
        }
        if (enc_ctx_->sample_aspect_ratio.num <= 0 || enc_ctx_->sample_aspect_ratio.den <= 0) {
            enc_ctx_->sample_aspect_ratio = AVRational{1, 1};
        }
        enc_ctx_->pix_fmt = FFmpegCommonUtils::chooseVideoPixelFormat(candidate, dec_ctx_->pix_fmt);
        enc_ctx_->time_base = av_inv_q(frame_rate_);
        if (enc_ctx_->time_base.num <= 0 || enc_ctx_->time_base.den <= 0) {
            enc_ctx_->time_base = video_input_stream->time_base;
        }
        if (enc_ctx_->time_base.num <= 0 || enc_ctx_->time_base.den <= 0) {
            enc_ctx_->time_base = AVRational{1, 25};
        }
        enc_ctx_->framerate = frame_rate_;
        enc_ctx_->bit_rate = video_input_stream->codecpar->bit_rate > 0
            ? video_input_stream->codecpar->bit_rate
            : 2 * 1000 * 1000;
        enc_ctx_->gop_size = frame_rate_.num > 0 && frame_rate_.den > 0
            ? frame_rate_.num / frame_rate_.den
            : 25;
        if (enc_ctx_->gop_size <= 0) enc_ctx_->gop_size = 25;
        enc_ctx_->max_b_frames = 0;

        if (!FFmpegCommonUtils::isHardwareVideoEncoder(candidate) &&
            (candidate->id == AV_CODEC_ID_H264 || candidate->id == AV_CODEC_ID_HEVC)) {
            av_opt_set(enc_ctx_->priv_data, "preset", "veryfast", 0);
        }

        if (output_format_->oformat->flags & AVFMT_GLOBALHEADER) {
            enc_ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }

        FFmpegCommonUtils::sanitizeVideoEncoderContextDefaults(enc_ctx_, candidate);
        return 0;
    };

    ret = configureEncoderContext(encoder);
    if (ret < 0) return ret;

    auto openEncoder = [&](const AVCodec *candidate) -> int {
        AVDictionary *codec_options = nullptr;
        if (FFmpegCommonUtils::isMediaCodecEncoder(candidate) &&
            (output_format_->oformat->flags & AVFMT_GLOBALHEADER)) {
            av_dict_set(&codec_options, "ndk_async", "false", 0);
        }
        const int open_ret = avcodec_open2(enc_ctx_, candidate, &codec_options);
        av_dict_free(&codec_options);
        return open_ret;
    };

    ret = FFmpegCommonUtils::reopenVideoEncoderWithFallback(
        "video encoder",
        &encoder,
        FFmpegCommonUtils::findFallbackSoftwareVideoEncoder(preferred_codec_id),
        openEncoder(encoder),
        configureEncoderContext,
        openEncoder
    );
    if (ret < 0) return ret;

    const std::vector<uint8_t> previous_extradata = video_extradata_snapshot_;
    ret = avcodec_parameters_from_context(video_output_stream->codecpar, enc_ctx_);
    if (ret < 0) return ret;
    video_output_stream->codecpar->codec_tag = 0;
    if (!output_format_->pb ||
        video_output_stream->time_base.num <= 0 ||
        video_output_stream->time_base.den <= 0) {
        video_output_stream->time_base = enc_ctx_->time_base;
    }
    video_output_stream->avg_frame_rate = enc_ctx_->framerate;
    video_output_stream->sample_aspect_ratio = enc_ctx_->sample_aspect_ratio;
    video_output_stream->disposition = video_input_stream->disposition;
    av_dict_copy(&video_output_stream->metadata, video_input_stream->metadata, 0);

    ret = FFmpegCommonUtils::copyDisplayMatrixSideData(video_input_stream, video_output_stream);
    if (ret < 0) return ret;

    video_extradata_snapshot_.clear();
    if (video_output_stream->codecpar->extradata && video_output_stream->codecpar->extradata_size > 0) {
        video_extradata_snapshot_.assign(
            video_output_stream->codecpar->extradata,
            video_output_stream->codecpar->extradata + video_output_stream->codecpar->extradata_size
        );
    }
    if (video_packets_written_ &&
        previous_extradata != video_extradata_snapshot_ &&
        !video_muxer_reopen_required_) {
        video_muxer_reopen_required_ = true;
        AWESOME_FF_LOGW(
            "HLS watermark encoder extradata changed after rebuild. Refreshed stream parameters and marked muxer state dirty."
        );
    }

    if (filter_graph_) avfilter_graph_free(&filter_graph_);
    filter_graph_ = nullptr;
    buffer_src_ctx_ = nullptr;
    buffer_sink_ctx_ = nullptr;

    ret = initializeFilterGraph(video_input_stream, watermark_);
    if (ret < 0) return ret;

    return 0;
}

void FFmpegWatermarkPipeline::markEncoderRebuilt() {
    video_encoder_input_origin_pts_ = AV_NOPTS_VALUE;
    video_encoder_output_offset_pts_ = AV_NOPTS_VALUE;
    video_force_keyframe_on_next_frame_ = true;
}

int FFmpegWatermarkPipeline::initialize(
    AVFormatContext *input_format,
    AVFormatContext *output_format,
    int video_input_index,
    int video_output_index,
    const FFmpegWatermarkConfig *watermark
) {
    int ret = 0;
    AVStream *video_input_stream = input_format->streams[video_input_index];
    const AVCodec *decoder = avcodec_find_decoder(video_input_stream->codecpar->codec_id);

    if (!decoder) return AVERROR_DECODER_NOT_FOUND;

    cleanup();
    input_stream_index_ = video_input_index;
    output_stream_index_ = video_output_index;
    input_format_ = input_format;
    output_format_ = output_format;
    watermark_ = watermark;
    frame_rate_ = av_guess_frame_rate(input_format, video_input_stream, nullptr);

    dec_ctx_ = avcodec_alloc_context3(decoder);
    if (!dec_ctx_) return AVERROR(ENOMEM);

    ret = avcodec_parameters_to_context(dec_ctx_, video_input_stream->codecpar);
    if (ret < 0) return ret;
    dec_ctx_->pkt_timebase = video_input_stream->time_base;

    ret = avcodec_open2(dec_ctx_, decoder, nullptr);
    if (ret < 0) return ret;

    ret = configureEncoder();
    if (ret < 0) return ret;

    dec_frame_ = av_frame_alloc();
    filtered_frame_ = av_frame_alloc();
    enc_packet_ = av_packet_alloc();
    if (!dec_frame_ || !filtered_frame_ || !enc_packet_) {
        return AVERROR(ENOMEM);
    }

    return 0;
}

int FFmpegWatermarkPipeline::writeEncodedVideoPackets(AVFormatContext *output_format) {
    int ret = 0;
    AVStream *output_stream = output_format->streams[output_stream_index_];
    video_last_write_packet_count_ = 0;

    while ((ret = avcodec_receive_packet(enc_ctx_, enc_packet_)) >= 0) {
        enc_packet_->stream_index = output_stream_index_;
        av_packet_rescale_ts(enc_packet_, enc_ctx_->time_base, output_stream->time_base);
        const int64_t minimum_next_dts = minimumNextVideoDts(output_stream, video_last_written_dts_);
        if (enc_packet_->dts == AV_NOPTS_VALUE && enc_packet_->pts != AV_NOPTS_VALUE) {
            enc_packet_->dts = enc_packet_->pts;
        }
        if (enc_packet_->pts == AV_NOPTS_VALUE && enc_packet_->dts != AV_NOPTS_VALUE) {
            enc_packet_->pts = enc_packet_->dts;
        }
        if (video_encoder_output_offset_pts_ == AV_NOPTS_VALUE) {
            int64_t target_start = 0;
            if (video_encoder_input_origin_pts_ != AV_NOPTS_VALUE) {
                target_start = av_rescale_q(
                    video_encoder_input_origin_pts_,
                    enc_ctx_->time_base,
                    output_stream->time_base
                );
            }
            if (minimum_next_dts != AV_NOPTS_VALUE && target_start < minimum_next_dts) {
                target_start = minimum_next_dts;
            }

            video_encoder_output_offset_pts_ = target_start - packetTimestampAnchor(enc_packet_);
        }
        shiftPacketTimestamps(enc_packet_, video_encoder_output_offset_pts_);
        if (minimum_next_dts != AV_NOPTS_VALUE &&
            enc_packet_->dts != AV_NOPTS_VALUE &&
            enc_packet_->dts < minimum_next_dts) {
            const int64_t shift = minimum_next_dts - enc_packet_->dts;
            video_encoder_output_offset_pts_ += shift;
            shiftPacketTimestamps(enc_packet_, shift);
            AWESOME_FF_LOGW(
                "Detected HLS watermark encoder timestamp reset. Rebasing continued output by %lld ticks (target dts=%lld).",
                static_cast<long long>(shift),
                static_cast<long long>(minimum_next_dts)
            );
        }
        if (enc_packet_->dts == AV_NOPTS_VALUE && minimum_next_dts != AV_NOPTS_VALUE) {
            enc_packet_->dts = minimum_next_dts;
        }
        if (enc_packet_->pts == AV_NOPTS_VALUE && enc_packet_->dts != AV_NOPTS_VALUE) {
            enc_packet_->pts = enc_packet_->dts;
        }
        if (enc_packet_->pts != AV_NOPTS_VALUE &&
            enc_packet_->dts != AV_NOPTS_VALUE &&
            enc_packet_->pts < enc_packet_->dts) {
            enc_packet_->pts = enc_packet_->dts;
        }
        if (enc_packet_->pts != AV_NOPTS_VALUE &&
            video_last_written_pts_ != AV_NOPTS_VALUE &&
            enc_packet_->pts <= video_last_written_pts_) {
            int64_t minimum_pts = video_last_written_pts_ + 1;
            if (enc_packet_->dts != AV_NOPTS_VALUE && minimum_pts < enc_packet_->dts) {
                minimum_pts = enc_packet_->dts;
            }
            enc_packet_->pts = minimum_pts;
        }
        enc_packet_->pos = -1;

        ret = av_interleaved_write_frame(output_format, enc_packet_);
        if (ret >= 0) {
            video_packets_written_ = true;
            video_last_write_packet_count_ += 1;
            video_last_written_pts_ = enc_packet_->pts != AV_NOPTS_VALUE ? enc_packet_->pts : enc_packet_->dts;
            video_last_written_dts_ = enc_packet_->dts != AV_NOPTS_VALUE ? enc_packet_->dts : enc_packet_->pts;
        }
        av_packet_unref(enc_packet_);
        if (ret < 0) {
            if (video_muxer_reopen_required_) {
                AWESOME_FF_LOGW(
                    "Muxer rejected continued HLS watermark packets after encoder rebuild. Current stream would require muxer reopen."
                );
            }
            return ret;
        }
    }

    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return 0;
    return ret;
}

int FFmpegWatermarkPipeline::sendFilteredFrameToEncoder(AVFrame *frame, AVFormatContext *output_format) {
    if (frame && frame->pts != AV_NOPTS_VALUE) {
        frame->pts = av_rescale_q(frame->pts, filter_time_base_, enc_ctx_->time_base);
    }

    if (frame) {
        prepareVideoFrameForEncoding(
            frame,
            frame->pts,
            &video_encoder_input_origin_pts_,
            &video_force_keyframe_on_next_frame_
        );
    }

    int ret = avcodec_send_frame(enc_ctx_, frame);
    if (ret < 0) return ret;
    return writeEncodedVideoPackets(output_format);
}

int FFmpegWatermarkPipeline::drainFilterSink(
    AVFormatContext *output_format,
    FFmpegProgressTracker *progress
) {
    int ret = 0;

    while ((ret = av_buffersink_get_frame(buffer_sink_ctx_, filtered_frame_)) >= 0) {
        int64_t timestamp = filtered_frame_->pts;
        if (timestamp == AV_NOPTS_VALUE) timestamp = filtered_frame_->best_effort_timestamp;
        if (progress) progress->update(timestamp, filter_time_base_);

        filtered_frame_->pict_type = AV_PICTURE_TYPE_NONE;
        ret = sendFilteredFrameToEncoder(filtered_frame_, output_format);
        av_frame_unref(filtered_frame_);
        if (ret < 0) return ret;
    }

    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return 0;
    return ret;
}

int FFmpegWatermarkPipeline::pushFrameToFilterAndEncode(
    AVFrame *frame,
    AVFormatContext *output_format,
    FFmpegProgressTracker *progress
) {
    int ret = av_buffersrc_add_frame_flags(
        buffer_src_ctx_,
        frame,
        frame ? AV_BUFFERSRC_FLAG_KEEP_REF : 0
    );
    if (ret < 0) return ret;

    video_last_write_packet_count_ = 0;
    ret = drainFilterSink(output_format, progress);
    if (ret >= 0 ||
        !frame ||
        !FFmpegCommonUtils::shouldAttemptVideoEncoderRecovery(enc_ctx_, ret) ||
        video_last_write_packet_count_ > 0) {
        return ret;
    }

    char error_buffer[128];
    if (av_strerror(ret, error_buffer, sizeof(error_buffer)) < 0) {
        snprintf(error_buffer, sizeof(error_buffer), "error %d", ret);
    }
    AWESOME_FF_LOGW(
        "HLS watermark video encode failed on hardware encoder (%s). Rebuilding filter graph and retrying current frame.",
        error_buffer
    );

    ret = configureEncoder();
    if (ret < 0) return ret;
    markEncoderRebuilt();

    ret = av_buffersrc_add_frame_flags(
        buffer_src_ctx_,
        frame,
        frame ? AV_BUFFERSRC_FLAG_KEEP_REF : 0
    );
    if (ret < 0) return ret;

    video_last_write_packet_count_ = 0;
    return drainFilterSink(output_format, progress);
}

int FFmpegWatermarkPipeline::processPacket(
    AVPacket *packet,
    AVFormatContext *output_format,
    FFmpegProgressTracker *progress
) {
    int ret = avcodec_send_packet(dec_ctx_, packet);
    av_packet_unref(packet);
    if (ret < 0) return ret;

    while ((ret = avcodec_receive_frame(dec_ctx_, dec_frame_)) >= 0) {
        if (dec_frame_->pts == AV_NOPTS_VALUE) {
            dec_frame_->pts = dec_frame_->best_effort_timestamp;
        }

        ret = pushFrameToFilterAndEncode(dec_frame_, output_format, progress);
        av_frame_unref(dec_frame_);
        if (ret < 0) return ret;
    }

    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return 0;
    return ret;
}

int FFmpegWatermarkPipeline::flush(AVFormatContext *output_format, FFmpegProgressTracker *progress) {
    int ret = avcodec_send_packet(dec_ctx_, nullptr);
    if (ret < 0 && ret != AVERROR_EOF) return ret;

    while ((ret = avcodec_receive_frame(dec_ctx_, dec_frame_)) >= 0) {
        if (dec_frame_->pts == AV_NOPTS_VALUE) {
            dec_frame_->pts = dec_frame_->best_effort_timestamp;
        }

        ret = pushFrameToFilterAndEncode(dec_frame_, output_format, progress);
        av_frame_unref(dec_frame_);
        if (ret < 0) return ret;
    }
    if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) return ret;

    ret = pushFrameToFilterAndEncode(nullptr, output_format, progress);
    if (ret < 0) return ret;

    ret = avcodec_send_frame(enc_ctx_, nullptr);
    if (ret < 0 && ret != AVERROR_EOF) return ret;

    return writeEncodedVideoPackets(output_format);
}
#endif
