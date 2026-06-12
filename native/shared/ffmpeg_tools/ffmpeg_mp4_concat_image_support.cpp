#include "ffmpeg_mp4_concat_image_support.h"

#include "ffmpeg_common_utils.h"
#include "ffmpeg_watermark_filter_utils.h"

#include <cmath>

extern "C" {
#include <libavutil/display.h>
}

namespace {

const char *kRebuildVideoEncoderReason = "Rebuilding hardware video encoder after pause/resume.";

int normalizeClockwiseQuarterTurns(int turns) {
    turns %= 4;
    if (turns < 0) turns += 4;
    return turns;
}

int getInputDisplayMatrixClockwiseQuarterTurns(const AVStream *input_stream) {
    if (!input_stream || !input_stream->codecpar) return 0;

    const AVPacketSideData *display_matrix = av_packet_side_data_get(
        input_stream->codecpar->coded_side_data,
        input_stream->codecpar->nb_coded_side_data,
        AV_PKT_DATA_DISPLAYMATRIX
    );
    if (!display_matrix || !display_matrix->data || display_matrix->size < static_cast<int>(9 * sizeof(int32_t))) {
        return 0;
    }

    const double rotation_ccw = av_display_rotation_get(reinterpret_cast<const int32_t *>(display_matrix->data));
    if (rotation_ccw != rotation_ccw) return 0;

    return normalizeClockwiseQuarterTurns(static_cast<int>(lround(rotation_ccw / 90.0)));
}

void copyFrameColorProps(AVFrame *dst, const AVFrame *src) {
    if (!dst || !src) return;

    dst->sample_aspect_ratio = src->sample_aspect_ratio.num > 0 && src->sample_aspect_ratio.den > 0
        ? src->sample_aspect_ratio
        : AVRational{1, 1};
    dst->color_range = src->color_range;
    dst->color_primaries = src->color_primaries;
    dst->color_trc = src->color_trc;
    dst->colorspace = src->colorspace;
}

int allocateVideoFrame(
    enum AVPixelFormat format,
    int width,
    int height,
    const AVFrame *src_props,
    AVFrame **frame_out
) {
    AVFrame *frame = nullptr;
    int ret = 0;

    if (!frame_out || width <= 0 || height <= 0) return AVERROR(EINVAL);
    *frame_out = nullptr;

    frame = av_frame_alloc();
    if (!frame) return AVERROR(ENOMEM);

    frame->format = format;
    frame->width = width;
    frame->height = height;
    if (src_props) {
        copyFrameColorProps(frame, src_props);
    } else {
        frame->sample_aspect_ratio = AVRational{1, 1};
    }

    ret = av_frame_get_buffer(frame, 32);
    if (ret < 0) {
        av_frame_free(&frame);
        return ret;
    }

    ret = av_frame_make_writable(frame);
    if (ret < 0) {
        av_frame_free(&frame);
        return ret;
    }

    *frame_out = frame;
    return 0;
}

int convertFrame(
    SwsContext **sws_ctx,
    const AVFrame *src_frame,
    enum AVPixelFormat dst_format,
    int dst_width,
    int dst_height,
    AVFrame **dst_frame_out
) {
    AVFrame *dst_frame = nullptr;
    int ret = 0;

    if (!sws_ctx || !src_frame || !dst_frame_out) return AVERROR(EINVAL);

    ret = allocateVideoFrame(dst_format, dst_width, dst_height, src_frame, &dst_frame);
    if (ret < 0) return ret;

    *sws_ctx = sws_getCachedContext(
        *sws_ctx,
        src_frame->width,
        src_frame->height,
        static_cast<AVPixelFormat>(src_frame->format),
        dst_width,
        dst_height,
        dst_format,
        SWS_BICUBIC,
        nullptr,
        nullptr,
        nullptr
    );
    if (!*sws_ctx) {
        av_frame_free(&dst_frame);
        return AVERROR(ENOMEM);
    }

    const int scaled = sws_scale(
        *sws_ctx,
        src_frame->data,
        src_frame->linesize,
        0,
        src_frame->height,
        dst_frame->data,
        dst_frame->linesize
    );
    if (scaled <= 0) {
        av_frame_free(&dst_frame);
        return AVERROR_EXTERNAL;
    }

    *dst_frame_out = dst_frame;
    return 0;
}

int rotateBgraFrameClockwise(const AVFrame *src_frame, int clockwise_quarter_turns, AVFrame **dst_frame_out) {
    AVFrame *dst_frame = nullptr;
    const int normalized_turns = normalizeClockwiseQuarterTurns(clockwise_quarter_turns);
    const int dst_width = (normalized_turns & 1) != 0 ? src_frame->height : src_frame->width;
    const int dst_height = (normalized_turns & 1) != 0 ? src_frame->width : src_frame->height;

    if (!src_frame || !dst_frame_out) return AVERROR(EINVAL);
    if (src_frame->format != AV_PIX_FMT_BGRA || !src_frame->data[0]) return AVERROR(EINVAL);

    int ret = allocateVideoFrame(AV_PIX_FMT_BGRA, dst_width, dst_height, src_frame, &dst_frame);
    if (ret < 0) return ret;

    for (int src_y = 0; src_y < src_frame->height; ++src_y) {
        const uint8_t *src_row = src_frame->data[0] + src_y * src_frame->linesize[0];
        for (int src_x = 0; src_x < src_frame->width; ++src_x) {
            int dst_x = 0;
            int dst_y = 0;

            switch (normalized_turns) {
                case 0:
                    dst_x = src_x;
                    dst_y = src_y;
                    break;
                case 1:
                    dst_x = src_frame->height - 1 - src_y;
                    dst_y = src_x;
                    break;
                case 2:
                    dst_x = src_frame->width - 1 - src_x;
                    dst_y = src_frame->height - 1 - src_y;
                    break;
                case 3:
                    dst_x = src_y;
                    dst_y = src_frame->width - 1 - src_x;
                    break;
                default:
                    av_frame_free(&dst_frame);
                    return AVERROR(EINVAL);
            }

            uint8_t *dst_pixel = dst_frame->data[0] + dst_y * dst_frame->linesize[0] + dst_x * 4;
            const uint8_t *src_pixel = src_row + src_x * 4;
            memcpy(dst_pixel, src_pixel, 4);
        }
    }

    *dst_frame_out = dst_frame;
    return 0;
}

int waitIfPaused(const FFmpegMp4ConcatImageRuntimeCallbacks &callbacks) {
    if (!callbacks.wait_if_paused) return 0;
    return callbacks.wait_if_paused();
}

int rebuildVideoEncoderAfterPause(
    const FFmpegMp4ConcatImageRuntimeCallbacks &callbacks,
    const char *reason
) {
    if (!callbacks.rebuild_video_encoder_after_pause) return 0;
    return callbacks.rebuild_video_encoder_after_pause(reason && reason[0] ? reason : kRebuildVideoEncoderReason);
}

bool isCancelled(const FFmpegMp4ConcatImageRuntimeCallbacks &callbacks) {
    if (!callbacks.is_cancelled) return false;
    return callbacks.is_cancelled();
}

} // namespace

int FFmpegMp4ConcatImageSupport::initializeConcatImageFrame(
    const char *image_path,
    AVFormatContext *input_format,
    int video_input_index,
    AVCodecContext *video_enc_ctx,
    SwsContext **sws_ctx,
    const char *rotation_log_context,
    AVFrame **concat_image_frame_out
) {
    AVFrame *decoded_image_frame = nullptr;
    AVFrame *bgra_image_frame = nullptr;
    AVFrame *rotated_image_frame = nullptr;
    AVRational image_time_base = AVRational{0, 1};
    const int input_rotation_compensation_turns = getInputDisplayMatrixClockwiseQuarterTurns(
        input_format ? input_format->streams[video_input_index] : nullptr
    );
    int scaled = 0;
    int ret = 0;

    if (!image_path || !image_path[0] || !video_enc_ctx || !sws_ctx || !concat_image_frame_out) {
        return AVERROR(EINVAL);
    }

    if (*concat_image_frame_out) {
        av_frame_free(concat_image_frame_out);
    }

    ret = FFmpegWatermarkFilterUtils::decodeImageFileToFrame(
        image_path,
        &decoded_image_frame,
        &image_time_base
    );
    if (ret < 0) return ret;

    ret = convertFrame(
        sws_ctx,
        decoded_image_frame,
        AV_PIX_FMT_BGRA,
        decoded_image_frame->width,
        decoded_image_frame->height,
        &bgra_image_frame
    );
    if (ret < 0) goto end;

    if (input_rotation_compensation_turns != 0) {
        AWESOME_FF_LOGI(
            "Applying %s rotation compensation: %d clockwise quarter-turn(s)",
            (rotation_log_context && rotation_log_context[0]) ? rotation_log_context : "concat image",
            input_rotation_compensation_turns
        );
    }

    ret = rotateBgraFrameClockwise(
        bgra_image_frame,
        input_rotation_compensation_turns,
        &rotated_image_frame
    );
    if (ret < 0) goto end;

    ret = allocateVideoFrame(
        video_enc_ctx->pix_fmt,
        video_enc_ctx->width,
        video_enc_ctx->height,
        decoded_image_frame,
        concat_image_frame_out
    );
    if (ret < 0) goto end;

    (*concat_image_frame_out)->sample_aspect_ratio = video_enc_ctx->sample_aspect_ratio;

    *sws_ctx = sws_getCachedContext(
        *sws_ctx,
        rotated_image_frame->width,
        rotated_image_frame->height,
        static_cast<AVPixelFormat>(rotated_image_frame->format),
        video_enc_ctx->width,
        video_enc_ctx->height,
        video_enc_ctx->pix_fmt,
        SWS_BICUBIC,
        nullptr,
        nullptr,
        nullptr
    );
    if (!*sws_ctx) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    scaled = sws_scale(
        *sws_ctx,
        rotated_image_frame->data,
        rotated_image_frame->linesize,
        0,
        rotated_image_frame->height,
        (*concat_image_frame_out)->data,
        (*concat_image_frame_out)->linesize
    );
    if (scaled <= 0) {
        ret = AVERROR_EXTERNAL;
        goto end;
    }

    (*concat_image_frame_out)->pts = 0;
    (*concat_image_frame_out)->pict_type = AV_PICTURE_TYPE_NONE;
    (*concat_image_frame_out)->color_range = decoded_image_frame->color_range;
    (*concat_image_frame_out)->color_primaries = decoded_image_frame->color_primaries;
    (*concat_image_frame_out)->color_trc = decoded_image_frame->color_trc;
    (*concat_image_frame_out)->colorspace = decoded_image_frame->colorspace;

    if ((*concat_image_frame_out)->color_range == AVCOL_RANGE_UNSPECIFIED) {
        (*concat_image_frame_out)->color_range = video_enc_ctx->color_range;
    }
    if ((*concat_image_frame_out)->color_primaries == AVCOL_PRI_UNSPECIFIED) {
        (*concat_image_frame_out)->color_primaries = video_enc_ctx->color_primaries;
    }
    if ((*concat_image_frame_out)->color_trc == AVCOL_TRC_UNSPECIFIED) {
        (*concat_image_frame_out)->color_trc = video_enc_ctx->color_trc;
    }
    if ((*concat_image_frame_out)->colorspace == AVCOL_SPC_UNSPECIFIED) {
        (*concat_image_frame_out)->colorspace = video_enc_ctx->colorspace;
    }

    ret = 0;

end:
    if (ret < 0 && concat_image_frame_out && *concat_image_frame_out) {
        av_frame_free(concat_image_frame_out);
    }
    av_frame_free(&rotated_image_frame);
    av_frame_free(&bgra_image_frame);
    av_frame_free(&decoded_image_frame);
    return ret;
}

int FFmpegMp4ConcatImageSupport::generateConcatImageVideoSegment(
    AVCodecContext **video_enc_ctx,
    AVFrame **video_enc_frame,
    AVFrame **concat_image_frame,
    int64_t concat_image_duration_us,
    int64_t start_time_us,
    int64_t *next_video_pts,
    const FFmpegMp4ConcatImageRuntimeCallbacks &callbacks
) {
    if (!video_enc_ctx || !*video_enc_ctx || !video_enc_frame || !*video_enc_frame ||
        !concat_image_frame || !*concat_image_frame || !next_video_pts || concat_image_duration_us <= 0) {
        return 0;
    }

    int64_t start_pts = av_rescale_q_rnd(start_time_us, AV_TIME_BASE_Q, (*video_enc_ctx)->time_base, AV_ROUND_UP);
    if (start_pts < *next_video_pts) start_pts = *next_video_pts;

    int64_t frame_count = av_rescale_q_rnd(
        concat_image_duration_us,
        AV_TIME_BASE_Q,
        (*video_enc_ctx)->time_base,
        AV_ROUND_UP
    );
    if (frame_count <= 0) frame_count = 1;

    for (int64_t index = 0; index < frame_count; ++index) {
        if (isCancelled(callbacks)) return 0;

        int ret = waitIfPaused(callbacks);
        if (ret < 0) return ret;

        ret = rebuildVideoEncoderAfterPause(callbacks, kRebuildVideoEncoderReason);
        if (ret < 0) return ret;

        if (!*video_enc_ctx || !*video_enc_frame || !*concat_image_frame) return AVERROR(EINVAL);

        ret = av_frame_make_writable(*video_enc_frame);
        if (ret < 0) return ret;

        ret = av_frame_copy(*video_enc_frame, *concat_image_frame);
        if (ret < 0) return ret;

        ret = av_frame_copy_props(*video_enc_frame, *concat_image_frame);
        if (ret < 0) return ret;

        (*video_enc_frame)->sample_aspect_ratio = (*concat_image_frame)->sample_aspect_ratio.num > 0 &&
                (*concat_image_frame)->sample_aspect_ratio.den > 0
            ? (*concat_image_frame)->sample_aspect_ratio
            : (*video_enc_ctx)->sample_aspect_ratio;
        (*video_enc_frame)->color_range = (*concat_image_frame)->color_range;
        (*video_enc_frame)->color_primaries = (*concat_image_frame)->color_primaries;
        (*video_enc_frame)->color_trc = (*concat_image_frame)->color_trc;
        (*video_enc_frame)->colorspace = (*concat_image_frame)->colorspace;

        if (!callbacks.send_prepared_video_frame) return AVERROR(EINVAL);
        ret = callbacks.send_prepared_video_frame(*video_enc_frame, start_pts + index);
        if (ret < 0) return ret;
    }

    return 0;
}

int FFmpegMp4ConcatImageSupport::appendSilentAudioSegment(
    AVCodecContext *audio_enc_ctx,
    AVAudioFifo *audio_fifo,
    int64_t duration_us,
    int64_t *audio_next_pts,
    const FFmpegMp4ConcatImageRuntimeCallbacks &callbacks
) {
    if (!audio_enc_ctx || !audio_fifo || !audio_next_pts || duration_us <= 0) return 0;

    int64_t remaining_samples = av_rescale_q_rnd(duration_us, AV_TIME_BASE_Q, audio_enc_ctx->time_base, AV_ROUND_UP);
    if (remaining_samples <= 0) return 0;

    const int fixed_frame_size = audio_enc_ctx->frame_size;
    const int variable_frame_size = audio_enc_ctx->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE;
    const int default_chunk_size = (!fixed_frame_size || variable_frame_size) ? 4096 : fixed_frame_size;

    if (*audio_next_pts == AV_NOPTS_VALUE) {
        *audio_next_pts = 0;
    }

    while (remaining_samples > 0) {
        int ret = waitIfPaused(callbacks);
        if (ret < 0) return ret;

        const int chunk_samples = remaining_samples > default_chunk_size
            ? default_chunk_size
            : static_cast<int>(remaining_samples);

        AVFrame *silence_frame = av_frame_alloc();
        if (!silence_frame) return AVERROR(ENOMEM);

        silence_frame->nb_samples = chunk_samples;
        silence_frame->format = audio_enc_ctx->sample_fmt;
        silence_frame->sample_rate = audio_enc_ctx->sample_rate;

        ret = av_channel_layout_copy(&silence_frame->ch_layout, &audio_enc_ctx->ch_layout);
        if (ret < 0) {
            av_frame_free(&silence_frame);
            return ret;
        }

        ret = av_frame_get_buffer(silence_frame, 0);
        if (ret < 0) {
            av_frame_free(&silence_frame);
            return ret;
        }

        av_samples_set_silence(
            silence_frame->data,
            0,
            silence_frame->nb_samples,
            audio_enc_ctx->ch_layout.nb_channels,
            audio_enc_ctx->sample_fmt
        );

        ret = av_audio_fifo_realloc(audio_fifo, av_audio_fifo_size(audio_fifo) + silence_frame->nb_samples);
        if (ret < 0) {
            av_frame_free(&silence_frame);
            return ret;
        }

        ret = av_audio_fifo_write(
            audio_fifo,
            reinterpret_cast<void **>(silence_frame->data),
            silence_frame->nb_samples
        );
        av_frame_free(&silence_frame);
        if (ret < chunk_samples) {
            return ret < 0 ? ret : AVERROR(EIO);
        }

        remaining_samples -= chunk_samples;

        if (!callbacks.drain_audio_fifo) return AVERROR(EINVAL);
        ret = callbacks.drain_audio_fifo(0);
        if (ret < 0) return ret;
    }

    return 0;
}

int64_t FFmpegMp4ConcatImageSupport::currentVideoOutputTimeUs(const AVCodecContext *video_enc_ctx, int64_t next_video_pts) {
    if (!video_enc_ctx || next_video_pts <= 0) return 0;
    return av_rescale_q(next_video_pts, video_enc_ctx->time_base, AV_TIME_BASE_Q);
}

int64_t FFmpegMp4ConcatImageSupport::currentAudioOutputTimeUs(
    const AVCodecContext *audio_enc_ctx,
    AVAudioFifo *audio_fifo,
    int64_t audio_next_pts
) {
    if (!audio_enc_ctx) return 0;

    int64_t pending_pts = audio_next_pts == AV_NOPTS_VALUE ? 0 : audio_next_pts;
    if (audio_fifo) {
        pending_pts += av_audio_fifo_size(audio_fifo);
    }

    if (pending_pts <= 0) return 0;
    return av_rescale_q(pending_pts, audio_enc_ctx->time_base, AV_TIME_BASE_Q);
}
