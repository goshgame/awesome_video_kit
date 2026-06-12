#include "ffmpeg_video_frame_extractor.h"

#include "ffmpeg_common_utils.h"
#include "ffmpeg_hls_network.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

extern "C" {
#include <libavutil/display.h>
}

namespace {

struct FrameRequest {
    double time_seconds;
    const char *output_path;
};

struct ExtractorContext {
    AVFormatContext *format = nullptr;
    AVCodecContext *decoder = nullptr;
    AVStream *video_stream = nullptr;
    AVFrame *decoded_frame = nullptr;
    AVPacket *packet = nullptr;
    const AVCodec *mjpeg_encoder = nullptr;
    AVCodecContext *mjpeg_encoder_context = nullptr;
    AVFrame *jpeg_frame = nullptr;
    AVFrame *scaled_frame = nullptr;
    AVPacket *jpeg_packet = nullptr;
    SwsContext *jpeg_sws = nullptr;
    AVPixelFormat jpeg_source_format = AV_PIX_FMT_NONE;
    int jpeg_source_width = 0;
    int jpeg_source_height = 0;
    int jpeg_scale_width = 0;
    int jpeg_scale_height = 0;
    int jpeg_output_width = 0;
    int jpeg_output_height = 0;
    int jpeg_rotation_quarter_turns = 0;
    int64_t jpeg_frame_index = 0;
    int requested_output_width = 0;
    int requested_output_height = 0;
    int video_stream_index = -1;
    int rotation_quarter_turns = 0;
    bool fast_mode = false;
};

void freeJpegEncoderResources(ExtractorContext *ctx) {
    if (!ctx) return;
    if (ctx->jpeg_sws) sws_freeContext(ctx->jpeg_sws);
    ctx->jpeg_sws = nullptr;
    if (ctx->jpeg_packet) av_packet_free(&ctx->jpeg_packet);
    if (ctx->scaled_frame) av_frame_free(&ctx->scaled_frame);
    if (ctx->jpeg_frame) av_frame_free(&ctx->jpeg_frame);
    if (ctx->mjpeg_encoder_context) avcodec_free_context(&ctx->mjpeg_encoder_context);
    ctx->jpeg_source_format = AV_PIX_FMT_NONE;
    ctx->jpeg_source_width = 0;
    ctx->jpeg_source_height = 0;
    ctx->jpeg_scale_width = 0;
    ctx->jpeg_scale_height = 0;
    ctx->jpeg_output_width = 0;
    ctx->jpeg_output_height = 0;
    ctx->jpeg_rotation_quarter_turns = 0;
    ctx->jpeg_frame_index = 0;
}

void freeExtractorContext(ExtractorContext *ctx) {
    if (!ctx) return;
    freeJpegEncoderResources(ctx);
    if (ctx->packet) av_packet_free(&ctx->packet);
    if (ctx->decoded_frame) av_frame_free(&ctx->decoded_frame);
    if (ctx->decoder) avcodec_free_context(&ctx->decoder);
    if (ctx->format) avformat_close_input(&ctx->format);
}

int openInputAndDecoder(const char *input_path, ExtractorContext *ctx) {
    avformat_network_init();

    AVDictionary *options = nullptr;
    FFmpegHlsNetworkUtils::setCommonNetworkOptions(&options);
    int ret = avformat_open_input(&ctx->format, input_path, nullptr, &options);
    av_dict_free(&options);
    if (ret < 0) {
        FFmpegCommonUtils::printError("capture avformat_open_input error", ret);
        return ret;
    }

    ret = avformat_find_stream_info(ctx->format, nullptr);
    if (ret < 0) {
        FFmpegCommonUtils::printError("capture avformat_find_stream_info error", ret);
        return ret;
    }

    ret = av_find_best_stream(ctx->format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (ret < 0) {
        FFmpegCommonUtils::printError("capture av_find_best_stream video error", ret);
        return ret;
    }

    ctx->video_stream_index = ret;
    ctx->video_stream = ctx->format->streams[ctx->video_stream_index];
    const AVCodec *decoder = avcodec_find_decoder(ctx->video_stream->codecpar->codec_id);
    if (!decoder) {
        AWESOME_FF_LOGE("capture video decoder not found");
        return AVERROR_DECODER_NOT_FOUND;
    }

    ctx->decoder = avcodec_alloc_context3(decoder);
    if (!ctx->decoder) return AVERROR(ENOMEM);

    ret = avcodec_parameters_to_context(ctx->decoder, ctx->video_stream->codecpar);
    if (ret < 0) {
        FFmpegCommonUtils::printError("capture avcodec_parameters_to_context error", ret);
        return ret;
    }

    ret = avcodec_open2(ctx->decoder, decoder, nullptr);
    if (ret < 0) {
        FFmpegCommonUtils::printError("capture avcodec_open2 decoder error", ret);
        return ret;
    }

    return 0;
}

int allocateFrames(ExtractorContext *ctx) {
    ctx->decoded_frame = av_frame_alloc();
    ctx->packet = av_packet_alloc();
    if (!ctx->decoded_frame || !ctx->packet) return AVERROR(ENOMEM);
    return 0;
}

int normalizePositiveDegrees(int degrees) {
    degrees %= 360;
    if (degrees < 0) degrees += 360;
    return degrees;
}

int normalizeQuarterTurns(int turns) {
    turns %= 4;
    if (turns < 0) turns += 4;
    return turns;
}

int getStreamRotationQuarterTurns(const AVStream *stream) {
    if (!stream || !stream->codecpar) return 0;

    const AVPacketSideData *display_matrix = av_packet_side_data_get(
        stream->codecpar->coded_side_data,
        stream->codecpar->nb_coded_side_data,
        AV_PKT_DATA_DISPLAYMATRIX
    );
    if (display_matrix && display_matrix->data && display_matrix->size >= static_cast<int>(9 * sizeof(int32_t))) {
        const double rotation_ccw = av_display_rotation_get(reinterpret_cast<const int32_t *>(display_matrix->data));
        if (rotation_ccw == rotation_ccw) {
            return normalizeQuarterTurns(static_cast<int>(std::lround(rotation_ccw / 90.0)));
        }
    }

    AVDictionaryEntry *rotation_entry = av_dict_get(stream->metadata, "rotate", nullptr, 0);
    if (rotation_entry && rotation_entry->value && rotation_entry->value[0]) {
        char *end = nullptr;
        const long rotation = std::strtol(rotation_entry->value, &end, 10);
        if (end && *end == '\0') {
            return normalizeQuarterTurns(normalizePositiveDegrees(static_cast<int>(rotation)) / 90);
        }
    }

    return 0;
}

int writePacketToFile(const char *output_path, const AVPacket *packet) {
    if (!output_path || !packet || !packet->data || packet->size <= 0) return AVERROR(EINVAL);
    FILE *file = fopen(output_path, "wb");
    if (!file) {
        AWESOME_FF_LOGE("capture fopen output error: %s", output_path);
        return AVERROR(errno);
    }

    if (fwrite(packet->data, 1, static_cast<size_t>(packet->size), file) != static_cast<size_t>(packet->size)) {
        fclose(file);
        return AVERROR(EIO);
    }

    const int close_ret = fclose(file);
    if (close_ret != 0) return AVERROR(errno);
    return 0;
}

bool encoderSupportsPixelFormat(const AVCodec *encoder, AVPixelFormat pixel_format) {
    if (!encoder || !encoder->pix_fmts) return true;
    for (const AVPixelFormat *format = encoder->pix_fmts; *format != AV_PIX_FMT_NONE; ++format) {
        if (*format == pixel_format) return true;
    }
    return false;
}

bool isDeprecatedJpegPixelFormat(AVPixelFormat pixel_format) {
    switch (pixel_format) {
        case AV_PIX_FMT_YUVJ420P:
        case AV_PIX_FMT_YUVJ422P:
        case AV_PIX_FMT_YUVJ444P:
        case AV_PIX_FMT_YUVJ440P:
        case AV_PIX_FMT_YUVJ411P:
            return true;
        default:
            return false;
    }
}

AVPixelFormat normalizePixelFormatForSws(AVPixelFormat pixel_format) {
    switch (pixel_format) {
        case AV_PIX_FMT_YUVJ420P:
            return AV_PIX_FMT_YUV420P;
        case AV_PIX_FMT_YUVJ422P:
            return AV_PIX_FMT_YUV422P;
        case AV_PIX_FMT_YUVJ444P:
            return AV_PIX_FMT_YUV444P;
        case AV_PIX_FMT_YUVJ440P:
            return AV_PIX_FMT_YUV440P;
        case AV_PIX_FMT_YUVJ411P:
            return AV_PIX_FMT_YUV411P;
        default:
            return pixel_format;
    }
}

int sourceRangeForSws(const AVFrame *frame) {
    if (!frame) return 0;
    if (frame->color_range == AVCOL_RANGE_JPEG) return 1;
    const AVPixelFormat pixel_format = static_cast<AVPixelFormat>(frame->format);
    return isDeprecatedJpegPixelFormat(pixel_format) ? 1 : 0;
}

int colorRangeForSws(AVColorRange color_range) {
    return color_range == AVCOL_RANGE_JPEG ? 1 : 0;
}

void configureSwsColorRange(SwsContext *sws, const AVFrame *source_frame, AVColorRange output_color_range) {
    if (!sws || !source_frame) return;

    int *source_coefficients = nullptr;
    int *destination_coefficients = nullptr;
    int source_range = 0;
    int destination_range = 0;
    int brightness = 0;
    int contrast = 0;
    int saturation = 0;

    const int ret = sws_getColorspaceDetails(
        sws,
        &source_coefficients,
        &source_range,
        &destination_coefficients,
        &destination_range,
        &brightness,
        &contrast,
        &saturation
    );
    if (ret < 0) {
        FFmpegCommonUtils::printError("capture sws_getColorspaceDetails error", ret);
        return;
    }

    sws_setColorspaceDetails(
        sws,
        source_coefficients,
        sourceRangeForSws(source_frame),
        destination_coefficients,
        colorRangeForSws(output_color_range),
        brightness,
        contrast,
        saturation
    );
}

AVPixelFormat chooseMjpegPixelFormat(const AVCodec *encoder) {
    const AVPixelFormat preferred_formats[] = {
        AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_YUV422P,
        AV_PIX_FMT_YUV444P,
        AV_PIX_FMT_YUVJ420P,
        AV_PIX_FMT_YUVJ422P,
        AV_PIX_FMT_YUVJ444P,
        AV_PIX_FMT_NONE,
    };

    for (const AVPixelFormat *format = preferred_formats; *format != AV_PIX_FMT_NONE; ++format) {
        if (encoderSupportsPixelFormat(encoder, *format)) return *format;
    }
    return encoder && encoder->pix_fmts ? encoder->pix_fmts[0] : AV_PIX_FMT_YUV420P;
}

struct OutputSize {
    int width;
    int height;
};

int rescaleDimension(int source_dimension, int requested_dimension, int source_reference_dimension) {
    if (source_dimension <= 0 || requested_dimension <= 0 || source_reference_dimension <= 0) return 0;
    const int64_t scaled_dimension = av_rescale(
        static_cast<int64_t>(source_dimension),
        static_cast<int64_t>(requested_dimension),
        static_cast<int64_t>(source_reference_dimension)
    );
    if (scaled_dimension <= 0) return 1;
    return scaled_dimension > INT_MAX ? INT_MAX : static_cast<int>(scaled_dimension);
}

OutputSize resolveOutputSize(const ExtractorContext *ctx, const AVFrame *source_frame) {
    if (!ctx || !source_frame) return OutputSize{0, 0};

    const bool swaps_dimensions = normalizeQuarterTurns(ctx->rotation_quarter_turns) % 2 != 0;
    const int display_source_width = swaps_dimensions ? source_frame->height : source_frame->width;
    const int display_source_height = swaps_dimensions ? source_frame->width : source_frame->height;
    const int requested_width = ctx->requested_output_width;
    const int requested_height = ctx->requested_output_height;
    if (requested_width > 0 && requested_height > 0) {
        return OutputSize{requested_width, requested_height};
    }
    if (requested_width > 0) {
        return OutputSize{
            requested_width,
            rescaleDimension(display_source_height, requested_width, display_source_width),
        };
    }
    if (requested_height > 0) {
        return OutputSize{
            rescaleDimension(display_source_width, requested_height, display_source_height),
            requested_height,
        };
    }
    return OutputSize{display_source_width, display_source_height};
}

OutputSize resolveScaleSize(const ExtractorContext *ctx, const OutputSize &output_size) {
    const bool swaps_dimensions = ctx && normalizeQuarterTurns(ctx->rotation_quarter_turns) % 2 != 0;
    return swaps_dimensions
        ? OutputSize{output_size.height, output_size.width}
        : output_size;
}

int chooseScaleFlags(int output_width, int output_height) {
    if (output_width > 0 && output_height > 0 && output_width * output_height <= 320 * 180) {
        return SWS_FAST_BILINEAR;
    }
    return SWS_BILINEAR;
}

bool jpegEncoderResourcesMatch(const ExtractorContext *ctx, const AVFrame *source_frame) {
    if (!ctx || !source_frame || !ctx->mjpeg_encoder_context || !ctx->jpeg_frame || !ctx->jpeg_packet || !ctx->jpeg_sws) {
        return false;
    }

    const OutputSize output_size = resolveOutputSize(ctx, source_frame);
    return ctx->jpeg_source_width == source_frame->width &&
        ctx->jpeg_source_height == source_frame->height &&
        ctx->jpeg_source_format == static_cast<AVPixelFormat>(source_frame->format) &&
        ctx->jpeg_rotation_quarter_turns == normalizeQuarterTurns(ctx->rotation_quarter_turns) &&
        ctx->jpeg_output_width == output_size.width &&
        ctx->jpeg_output_height == output_size.height;
}

int ensureJpegEncoderResources(ExtractorContext *ctx, const AVFrame *source_frame) {
    if (!ctx || !source_frame || source_frame->width <= 0 || source_frame->height <= 0) {
        return AVERROR(EINVAL);
    }

    const OutputSize output_size = resolveOutputSize(ctx, source_frame);
    const OutputSize scale_size = resolveScaleSize(ctx, output_size);
    if (output_size.width <= 0 || output_size.height <= 0) {
        return AVERROR(EINVAL);
    }

    if (jpegEncoderResourcesMatch(ctx, source_frame)) {
        return 0;
    }

    freeJpegEncoderResources(ctx);

    if (!ctx->mjpeg_encoder) {
        ctx->mjpeg_encoder = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
        if (!ctx->mjpeg_encoder) {
            AWESOME_FF_LOGE("capture mjpeg encoder not found");
            return AVERROR_ENCODER_NOT_FOUND;
        }
    }

    ctx->mjpeg_encoder_context = avcodec_alloc_context3(ctx->mjpeg_encoder);
    ctx->jpeg_frame = av_frame_alloc();
    ctx->jpeg_packet = av_packet_alloc();
    if (normalizeQuarterTurns(ctx->rotation_quarter_turns) != 0) {
        ctx->scaled_frame = av_frame_alloc();
    }
    int ret = 0;

    if (!ctx->mjpeg_encoder_context || !ctx->jpeg_frame || !ctx->jpeg_packet ||
        (normalizeQuarterTurns(ctx->rotation_quarter_turns) != 0 && !ctx->scaled_frame)) {
        ret = AVERROR(ENOMEM);
        goto cleanup;
    }

    ctx->mjpeg_encoder_context->codec_id = AV_CODEC_ID_MJPEG;
    ctx->mjpeg_encoder_context->codec_type = AVMEDIA_TYPE_VIDEO;
    ctx->mjpeg_encoder_context->width = output_size.width;
    ctx->mjpeg_encoder_context->height = output_size.height;
    ctx->mjpeg_encoder_context->time_base = AVRational{1, 1};
    ctx->mjpeg_encoder_context->framerate = AVRational{1, 1};
    ctx->mjpeg_encoder_context->pix_fmt = chooseMjpegPixelFormat(ctx->mjpeg_encoder);
    ctx->mjpeg_encoder_context->color_range = AVCOL_RANGE_JPEG;

    ret = avcodec_open2(ctx->mjpeg_encoder_context, ctx->mjpeg_encoder, nullptr);
    if (ret < 0) {
        FFmpegCommonUtils::printError("capture avcodec_open2 mjpeg encoder error", ret);
        goto cleanup;
    }

    ctx->jpeg_frame->format = ctx->mjpeg_encoder_context->pix_fmt;
    ctx->jpeg_frame->width = ctx->mjpeg_encoder_context->width;
    ctx->jpeg_frame->height = ctx->mjpeg_encoder_context->height;
    ctx->jpeg_frame->color_range = AVCOL_RANGE_JPEG;

    ret = av_frame_get_buffer(ctx->jpeg_frame, 32);
    if (ret < 0) {
        FFmpegCommonUtils::printError("capture av_frame_get_buffer jpeg error", ret);
        goto cleanup;
    }

    if (ctx->scaled_frame) {
        ctx->scaled_frame->format = ctx->mjpeg_encoder_context->pix_fmt;
        ctx->scaled_frame->width = scale_size.width;
        ctx->scaled_frame->height = scale_size.height;
        ctx->scaled_frame->color_range = AVCOL_RANGE_JPEG;
        ret = av_frame_get_buffer(ctx->scaled_frame, 32);
        if (ret < 0) {
            FFmpegCommonUtils::printError("capture av_frame_get_buffer scaled jpeg error", ret);
            goto cleanup;
        }
    }

    ctx->jpeg_sws = sws_getContext(
        source_frame->width,
        source_frame->height,
        normalizePixelFormatForSws(static_cast<AVPixelFormat>(source_frame->format)),
        scale_size.width,
        scale_size.height,
        normalizePixelFormatForSws(ctx->mjpeg_encoder_context->pix_fmt),
        chooseScaleFlags(scale_size.width, scale_size.height),
        nullptr,
        nullptr,
        nullptr
    );
    if (!ctx->jpeg_sws) {
        AWESOME_FF_LOGE("capture sws_getContext mjpeg error");
        ret = AVERROR(EINVAL);
        goto cleanup;
    }
    configureSwsColorRange(ctx->jpeg_sws, source_frame, ctx->jpeg_frame->color_range);

    ctx->jpeg_source_width = source_frame->width;
    ctx->jpeg_source_height = source_frame->height;
    ctx->jpeg_source_format = static_cast<AVPixelFormat>(source_frame->format);
    ctx->jpeg_scale_width = scale_size.width;
    ctx->jpeg_scale_height = scale_size.height;
    ctx->jpeg_output_width = output_size.width;
    ctx->jpeg_output_height = output_size.height;
    ctx->jpeg_rotation_quarter_turns = normalizeQuarterTurns(ctx->rotation_quarter_turns);
    return 0;

cleanup:
    freeJpegEncoderResources(ctx);
    return ret;
}

int rotateFramePixels(const AVFrame *source_frame, AVFrame *destination_frame, int quarter_turns) {
    if (!source_frame || !destination_frame || source_frame->format != destination_frame->format) {
        return AVERROR(EINVAL);
    }

    const AVPixFmtDescriptor *descriptor = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(source_frame->format));
    if (!descriptor || descriptor->flags & AV_PIX_FMT_FLAG_BITSTREAM || descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL) {
        return AVERROR(EINVAL);
    }

    quarter_turns = normalizeQuarterTurns(quarter_turns);
    if (quarter_turns == 0) {
        return av_frame_copy(destination_frame, source_frame);
    }

    const int destination_planes = av_pix_fmt_count_planes(static_cast<AVPixelFormat>(destination_frame->format));
    const int source_width = source_frame->width;
    const int source_height = source_frame->height;
    const bool swaps_dimensions = quarter_turns % 2 != 0;

    if ((swaps_dimensions && (destination_frame->width != source_height || destination_frame->height != source_width)) ||
        (!swaps_dimensions && (destination_frame->width != source_width || destination_frame->height != source_height))) {
        return AVERROR(EINVAL);
    }

    for (int plane = 0; plane < destination_planes; ++plane) {
        const int horizontal_shift = plane == 0 ? 0 : descriptor->log2_chroma_w;
        const int vertical_shift = plane == 0 ? 0 : descriptor->log2_chroma_h;
        const int source_plane_width = AV_CEIL_RSHIFT(source_width, horizontal_shift);
        const int source_plane_height = AV_CEIL_RSHIFT(source_height, vertical_shift);
        const int destination_plane_width = AV_CEIL_RSHIFT(destination_frame->width, horizontal_shift);
        const int destination_plane_height = AV_CEIL_RSHIFT(destination_frame->height, vertical_shift);

        const uint8_t *source_data = source_frame->data[plane];
        uint8_t *destination_data = destination_frame->data[plane];
        if (!source_data || !destination_data) continue;

        for (int y = 0; y < source_plane_height; ++y) {
            for (int x = 0; x < source_plane_width; ++x) {
                int destination_x = x;
                int destination_y = y;
                switch (quarter_turns) {
                    case 1:
                        destination_x = y;
                        destination_y = destination_plane_height - 1 - x;
                        break;
                    case 2:
                        destination_x = destination_plane_width - 1 - x;
                        destination_y = destination_plane_height - 1 - y;
                        break;
                    case 3:
                        destination_x = destination_plane_width - 1 - y;
                        destination_y = x;
                        break;
                    default:
                        break;
                }

                destination_data[destination_y * destination_frame->linesize[plane] + destination_x] =
                    source_data[y * source_frame->linesize[plane] + x];
            }
        }
    }

    return 0;
}

int writeFrameToJpeg(ExtractorContext *ctx, AVFrame *source_frame, const char *output_path) {
    if (!ctx || !source_frame || !output_path || !output_path[0] || source_frame->width <= 0 || source_frame->height <= 0) {
        return AVERROR(EINVAL);
    }

    int ret = ensureJpegEncoderResources(ctx, source_frame);
    if (ret < 0) return ret;

    ret = av_frame_make_writable(ctx->jpeg_frame);
    if (ret < 0) {
        FFmpegCommonUtils::printError("capture av_frame_make_writable jpeg error", ret);
        return ret;
    }
    if (ctx->scaled_frame) {
        ret = av_frame_make_writable(ctx->scaled_frame);
        if (ret < 0) {
            FFmpegCommonUtils::printError("capture av_frame_make_writable scaled jpeg error", ret);
            return ret;
        }
    }

    ctx->jpeg_frame->pts = ctx->jpeg_frame_index++;
    av_packet_unref(ctx->jpeg_packet);

    AVFrame *scale_destination_frame = ctx->scaled_frame ? ctx->scaled_frame : ctx->jpeg_frame;
    if (sws_scale(
        ctx->jpeg_sws,
        source_frame->data,
        source_frame->linesize,
        0,
        source_frame->height,
        scale_destination_frame->data,
        scale_destination_frame->linesize
    ) != scale_destination_frame->height) {
        return AVERROR(EIO);
    }

    if (ctx->scaled_frame) {
        ret = rotateFramePixels(ctx->scaled_frame, ctx->jpeg_frame, ctx->jpeg_rotation_quarter_turns);
        if (ret < 0) {
            FFmpegCommonUtils::printError("capture rotate jpeg frame error", ret);
            return ret;
        }
    }

    ret = avcodec_send_frame(ctx->mjpeg_encoder_context, ctx->jpeg_frame);
    if (ret < 0) {
        FFmpegCommonUtils::printError("capture avcodec_send_frame mjpeg error", ret);
        return ret;
    }

    ret = avcodec_receive_packet(ctx->mjpeg_encoder_context, ctx->jpeg_packet);
    if (ret < 0) {
        FFmpegCommonUtils::printError("capture avcodec_receive_packet mjpeg error", ret);
        return ret;
    }

    ret = writePacketToFile(output_path, ctx->jpeg_packet);
    av_packet_unref(ctx->jpeg_packet);
    return ret;
}

int64_t frameTimestampUs(const AVFrame *frame, const AVStream *stream) {
    int64_t timestamp = frame->best_effort_timestamp;
    if (timestamp == AV_NOPTS_VALUE) timestamp = frame->pts;
    if (timestamp == AV_NOPTS_VALUE) return AV_NOPTS_VALUE;
    return av_rescale_q(timestamp, stream->time_base, AV_TIME_BASE_Q);
}

int seekToTime(ExtractorContext *ctx, double time_seconds) {
    const double clamped_seconds = time_seconds < 0.0 ? 0.0 : time_seconds;
    const int64_t target_us = static_cast<int64_t>(clamped_seconds * AV_TIME_BASE + 0.5);
    const int64_t stream_ts = av_rescale_q(target_us, AV_TIME_BASE_Q, ctx->video_stream->time_base);
    const int flags = AVSEEK_FLAG_BACKWARD;
    const int ret = av_seek_frame(ctx->format, ctx->video_stream_index, stream_ts, flags);
    if (ret < 0) {
        FFmpegCommonUtils::printError("capture av_seek_frame error", ret);
        return ret;
    }

    avcodec_flush_buffers(ctx->decoder);
    return 0;
}

int64_t requestTargetUs(const FrameRequest &request) {
    return static_cast<int64_t>((request.time_seconds < 0.0 ? 0.0 : request.time_seconds) * AV_TIME_BASE + 0.5);
}

int replaceBestFrame(AVFrame **best_frame, const AVFrame *decoded_frame) {
    AVFrame *clone = av_frame_clone(decoded_frame);
    if (!clone) return AVERROR(ENOMEM);
    if (*best_frame) av_frame_free(best_frame);
    *best_frame = clone;
    return 0;
}

int processDecodedFrameForRequests(
    ExtractorContext *ctx,
    const std::vector<FrameRequest> &requests,
    size_t *request_index,
    AVFrame **best_frame,
    int64_t *best_delta
) {
    if (!ctx || !request_index || !best_frame || !best_delta) return AVERROR(EINVAL);
    if (*request_index >= requests.size()) return 0;

    const int64_t frame_us = frameTimestampUs(ctx->decoded_frame, ctx->video_stream);
    if (frame_us == AV_NOPTS_VALUE) {
        if (!*best_frame) return replaceBestFrame(best_frame, ctx->decoded_frame);
        return 0;
    }

    if (ctx->fast_mode) {
        while (*request_index < requests.size() && frame_us >= requestTargetUs(requests[*request_index])) {
            const int ret = writeFrameToJpeg(ctx, ctx->decoded_frame, requests[*request_index].output_path);
            if (ret < 0) return ret;
            ++(*request_index);
        }
        return 0;
    }

    int ret = 0;
    while (*request_index < requests.size()) {
        const int64_t target_us = requestTargetUs(requests[*request_index]);
        const int64_t delta = llabs(frame_us - target_us);
        if (delta < *best_delta) {
            ret = replaceBestFrame(best_frame, ctx->decoded_frame);
            if (ret < 0) return ret;
            *best_delta = delta;
        }

        if (frame_us < target_us) break;

        if (!*best_frame) return AVERROR(EINVAL);
        ret = writeFrameToJpeg(ctx, *best_frame, requests[*request_index].output_path);
        if (ret < 0) return ret;

        av_frame_free(best_frame);
        *best_delta = INT64_MAX;
        ++(*request_index);
    }

    return 0;
}

int decodeAndCaptureFastRequest(ExtractorContext *ctx, const FrameRequest &request, volatile int *cancel_flag) {
    int ret = seekToTime(ctx, request.time_seconds);
    if (ret < 0) return ret;

    const int64_t target_us = requestTargetUs(request);
    AVFrame *fallback_frame = nullptr;

    while (!cancel_flag || !*cancel_flag) {
        ret = av_read_frame(ctx->format, ctx->packet);
        if (ret < 0) break;

        if (ctx->packet->stream_index != ctx->video_stream_index) {
            av_packet_unref(ctx->packet);
            continue;
        }

        ret = avcodec_send_packet(ctx->decoder, ctx->packet);
        av_packet_unref(ctx->packet);
        if (ret < 0 && ret != AVERROR(EAGAIN)) {
            FFmpegCommonUtils::printError("capture avcodec_send_packet error", ret);
            break;
        }

        while ((ret = avcodec_receive_frame(ctx->decoder, ctx->decoded_frame)) >= 0) {
            const int64_t frame_us = frameTimestampUs(ctx->decoded_frame, ctx->video_stream);
            if (frame_us == AV_NOPTS_VALUE || frame_us >= target_us) {
                ret = writeFrameToJpeg(ctx, ctx->decoded_frame, request.output_path);
                av_frame_unref(ctx->decoded_frame);
                if (fallback_frame) av_frame_free(&fallback_frame);
                return ret;
            }

            ret = replaceBestFrame(&fallback_frame, ctx->decoded_frame);
            av_frame_unref(ctx->decoded_frame);
            if (ret < 0) {
                if (fallback_frame) av_frame_free(&fallback_frame);
                return ret;
            }
        }

        if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
            FFmpegCommonUtils::printError("capture avcodec_receive_frame error", ret);
            break;
        }
    }

    if (!cancel_flag || !*cancel_flag) {
        ret = avcodec_send_packet(ctx->decoder, nullptr);
        if (ret >= 0 || ret == AVERROR_EOF) {
            while ((ret = avcodec_receive_frame(ctx->decoder, ctx->decoded_frame)) >= 0) {
                const int64_t frame_us = frameTimestampUs(ctx->decoded_frame, ctx->video_stream);
                if (frame_us == AV_NOPTS_VALUE || frame_us >= target_us) {
                    ret = writeFrameToJpeg(ctx, ctx->decoded_frame, request.output_path);
                    av_frame_unref(ctx->decoded_frame);
                    if (fallback_frame) av_frame_free(&fallback_frame);
                    return ret;
                }

                ret = replaceBestFrame(&fallback_frame, ctx->decoded_frame);
                av_frame_unref(ctx->decoded_frame);
                if (ret < 0) {
                    if (fallback_frame) av_frame_free(&fallback_frame);
                    return ret;
                }
            }
        }

        if (fallback_frame) {
            ret = writeFrameToJpeg(ctx, fallback_frame, request.output_path);
            av_frame_free(&fallback_frame);
            return ret;
        }
    }

    if (fallback_frame) av_frame_free(&fallback_frame);
    return cancel_flag && *cancel_flag ? AVERROR_EXIT : AVERROR_EOF;
}

int receiveAndCaptureAvailableFrames(
    ExtractorContext *ctx,
    const std::vector<FrameRequest> &requests,
    size_t *request_index,
    AVFrame **best_frame,
    int64_t *best_delta
) {
    int ret = 0;
    while ((ret = avcodec_receive_frame(ctx->decoder, ctx->decoded_frame)) >= 0) {
        ret = processDecodedFrameForRequests(ctx, requests, request_index, best_frame, best_delta);
        av_frame_unref(ctx->decoded_frame);
        if (ret < 0 || *request_index >= requests.size()) return ret;
    }

    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return 0;
    FFmpegCommonUtils::printError("capture avcodec_receive_frame error", ret);
    return ret;
}

int flushRemainingFramesAndCapture(
    ExtractorContext *ctx,
    const std::vector<FrameRequest> &requests,
    size_t *request_index,
    AVFrame **best_frame,
    int64_t *best_delta
) {
    int ret = avcodec_send_packet(ctx->decoder, nullptr);
    if (ret < 0 && ret != AVERROR_EOF) return ret;

    ret = receiveAndCaptureAvailableFrames(ctx, requests, request_index, best_frame, best_delta);
    if (ret < 0) return ret;

    while (*request_index < requests.size() && *best_frame) {
        ret = writeFrameToJpeg(ctx, *best_frame, requests[*request_index].output_path);
        if (ret < 0) return ret;
        ++(*request_index);
    }

    return *request_index >= requests.size() ? 0 : AVERROR_EOF;
}

int decodeAndCaptureSortedRequests(ExtractorContext *ctx, const std::vector<FrameRequest> &requests, volatile int *cancel_flag) {
    if (!ctx || requests.empty()) return AVERROR(EINVAL);

    int ret = seekToTime(ctx, requests.front().time_seconds);
    if (ret < 0) return ret;

    size_t request_index = 0;
    AVFrame *best_frame = nullptr;
    int64_t best_delta = INT64_MAX;

    while (request_index < requests.size() && (!cancel_flag || !*cancel_flag)) {
        ret = av_read_frame(ctx->format, ctx->packet);
        if (ret < 0) break;

        if (ctx->packet->stream_index != ctx->video_stream_index) {
            av_packet_unref(ctx->packet);
            continue;
        }

        ret = avcodec_send_packet(ctx->decoder, ctx->packet);
        av_packet_unref(ctx->packet);
        if (ret < 0 && ret != AVERROR(EAGAIN)) {
            FFmpegCommonUtils::printError("capture avcodec_send_packet error", ret);
            break;
        }

        ret = receiveAndCaptureAvailableFrames(ctx, requests, &request_index, &best_frame, &best_delta);
        if (ret < 0) break;
    }

    if (ret >= 0 || ret == AVERROR_EOF) {
        ret = flushRemainingFramesAndCapture(ctx, requests, &request_index, &best_frame, &best_delta);
    }

    if (best_frame) av_frame_free(&best_frame);
    if (cancel_flag && *cancel_flag) return AVERROR_EXIT;
    return ret < 0 ? ret : 0;
}

int decodeAndCaptureRequestGroups(ExtractorContext *ctx, const std::vector<FrameRequest> &requests, volatile int *cancel_flag) {
    if (!ctx || requests.empty()) return AVERROR(EINVAL);

    if (ctx->fast_mode) {
        for (const FrameRequest &request : requests) {
            if (cancel_flag && *cancel_flag) return AVERROR_EXIT;
            const int ret = decodeAndCaptureFastRequest(ctx, request, cancel_flag);
            if (ret < 0) return ret;
        }
        return 0;
    }

    constexpr int64_t kMaxSequentialDecodeGapUs = AV_TIME_BASE / 2;
    int ret = 0;
    size_t group_begin = 0;
    while (group_begin < requests.size()) {
        if (cancel_flag && *cancel_flag) return AVERROR_EXIT;

        size_t group_end = group_begin + 1;
        while (group_end < requests.size()) {
            const int64_t previous_us = requestTargetUs(requests[group_end - 1]);
            const int64_t current_us = requestTargetUs(requests[group_end]);
            if (current_us - previous_us > kMaxSequentialDecodeGapUs) break;
            ++group_end;
        }

        std::vector<FrameRequest> group_requests(
            requests.begin() + static_cast<std::vector<FrameRequest>::difference_type>(group_begin),
            requests.begin() + static_cast<std::vector<FrameRequest>::difference_type>(group_end)
        );
        ret = decodeAndCaptureSortedRequests(ctx, group_requests, cancel_flag);
        if (ret < 0) return ret;
        group_begin = group_end;
    }

    return 0;
}

} // namespace

int capture_video_frames_to_jpegs_cpp(
    const char *input_path,
    const double *times_seconds,
    const char *const *output_paths,
    int count,
    int output_width,
    int output_height,
    int fast_mode,
    volatile int *cancel_flag
) {
    FFmpegCommonUtils::installPlatformLogBridge();

    if (!input_path || !input_path[0] || !times_seconds || !output_paths || count <= 0 || output_width < 0 || output_height < 0) {
        return AVERROR(EINVAL);
    }

    std::vector<FrameRequest> requests;
    requests.reserve(static_cast<size_t>(count));
    for (int index = 0; index < count; ++index) {
        if (!output_paths[index] || !output_paths[index][0]) return AVERROR(EINVAL);
        requests.push_back(FrameRequest{times_seconds[index], output_paths[index]});
    }

    std::sort(requests.begin(), requests.end(), [](const FrameRequest &lhs, const FrameRequest &rhs) {
        return lhs.time_seconds < rhs.time_seconds;
    });

    ExtractorContext ctx;
    ctx.requested_output_width = output_width;
    ctx.requested_output_height = output_height;
    ctx.fast_mode = fast_mode != 0;
    int ret = openInputAndDecoder(input_path, &ctx);
    if (ret >= 0) ctx.rotation_quarter_turns = getStreamRotationQuarterTurns(ctx.video_stream);
    if (ret >= 0) ret = allocateFrames(&ctx);
    if (ret >= 0) ret = decodeAndCaptureRequestGroups(&ctx, requests, cancel_flag);

    freeExtractorContext(&ctx);
    return ret < 0 ? ret : 0;
}
