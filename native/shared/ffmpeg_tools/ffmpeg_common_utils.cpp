#include "ffmpeg_common_utils.h"

#include <cstdarg>
#include <mutex>
#include <vector>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

#if defined(__APPLE__) && __has_include(<TargetConditionals.h>)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && __has_include(<os/log.h>)
#include <os/log.h>
#endif

namespace {

const enum AVCodecID kVideoEncoderFallbacks[] = {
    AV_CODEC_ID_H264,
    AV_CODEC_ID_HEVC,
    AV_CODEC_ID_MPEG4,
};

const char *kPlatformLogTag = "AwesomeVideoKit";

int normalizeLogLevel(int ffmpeg_log_level) {
    return ffmpeg_log_level & 0xFF;
}

void trimTrailingLineEndings(std::string *message) {
    if (!message) return;
    while (!message->empty()) {
        const char tail = message->back();
        if (tail != '\n' && tail != '\r') break;
        message->pop_back();
    }
}

std::string formatLogMessage(const char *format, va_list args) {
    if (!format || !format[0]) return {};

    va_list args_copy;
    va_copy(args_copy, args);
    const int required_length = vsnprintf(nullptr, 0, format, args_copy);
    va_end(args_copy);
    if (required_length <= 0) return {};

    std::vector<char> buffer(static_cast<size_t>(required_length) + 1);
    vsnprintf(buffer.data(), buffer.size(), format, args);
    return std::string(buffer.data(), static_cast<size_t>(required_length));
}

#if defined(__ANDROID__)
int androidLogPriorityForLevel(int ffmpeg_log_level) {
    const int level = normalizeLogLevel(ffmpeg_log_level);
    if (level <= AV_LOG_PANIC) return ANDROID_LOG_FATAL;
    if (level <= AV_LOG_ERROR) return ANDROID_LOG_ERROR;
    if (level <= AV_LOG_WARNING) return ANDROID_LOG_WARN;
    if (level <= AV_LOG_INFO) return ANDROID_LOG_INFO;
    return ANDROID_LOG_DEBUG;
}
#elif defined(__APPLE__) && __has_include(<os/log.h>)
os_log_type_t appleLogTypeForLevel(int ffmpeg_log_level) {
    const int level = normalizeLogLevel(ffmpeg_log_level);
    if (level <= AV_LOG_PANIC) return OS_LOG_TYPE_FAULT;
    if (level <= AV_LOG_ERROR) return OS_LOG_TYPE_ERROR;
    if (level <= AV_LOG_WARNING) return OS_LOG_TYPE_DEFAULT;
    if (level <= AV_LOG_INFO) return OS_LOG_TYPE_INFO;
    return OS_LOG_TYPE_DEBUG;
}

os_log_t goshPlatformLogger() {
    static os_log_t logger = os_log_create("com.goshlive.awesomevideokit", "native");
    return logger;
}
#endif

void dispatchPlatformLog(int ffmpeg_log_level, const char *message) {
    if (!message || !message[0]) return;

    std::string line(message);
    trimTrailingLineEndings(&line);
    if (line.empty()) return;

#if defined(__ANDROID__)
    __android_log_write(androidLogPriorityForLevel(ffmpeg_log_level), kPlatformLogTag, line.c_str());
#elif defined(__APPLE__) && __has_include(<os/log.h>)
    os_log_with_type(goshPlatformLogger(), appleLogTypeForLevel(ffmpeg_log_level), "%{public}s", line.c_str());
#else
    fprintf(stderr, "[%s] %s\n", kPlatformLogTag, line.c_str());
#endif
}

void ffmpegPlatformLogCallback(void *avcl, int ffmpeg_log_level, const char *format, va_list args) {
    if (normalizeLogLevel(ffmpeg_log_level) > av_log_get_level()) return;

    char line_buffer[1024] = {0};
    thread_local int print_prefix = 1;

    va_list args_copy;
    va_copy(args_copy, args);
    av_log_format_line2(
        avcl,
        ffmpeg_log_level,
        format,
        args_copy,
        line_buffer,
        sizeof(line_buffer),
        &print_prefix
    );
    va_end(args_copy);

    dispatchPlatformLog(ffmpeg_log_level, line_buffer);
}

bool canPreferVideoToolboxEncoder() {
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE && defined(TARGET_OS_SIMULATOR) && !TARGET_OS_SIMULATOR
    return true;
#else
    return false;
#endif
}

bool canPreferMediaCodecEncoder() {
#if defined(__ANDROID__)
    return true;
#else
    return false;
#endif
}

const char *hardwareVideoEncoderName(enum AVCodecID codec_id) {
#if defined(__ANDROID__)
    switch (codec_id) {
        case AV_CODEC_ID_H264:
            return "h264_mediacodec";
        case AV_CODEC_ID_HEVC:
            return "hevc_mediacodec";
        case AV_CODEC_ID_MPEG4:
            return "mpeg4_mediacodec";
        default:
            return nullptr;
    }
#else
    switch (codec_id) {
        case AV_CODEC_ID_H264:
            return "h264_videotoolbox";
        case AV_CODEC_ID_HEVC:
            return "hevc_videotoolbox";
        default:
            return nullptr;
    }
#endif
}

const char *softwareVideoEncoderName(enum AVCodecID codec_id) {
    switch (codec_id) {
        case AV_CODEC_ID_H264:
            return "libx264";
        case AV_CODEC_ID_HEVC:
            return "libx265";
        default:
            return nullptr;
    }
}

const AVCodec *findVideoEncoderById(enum AVCodecID codec_id) {
    const AVCodec *encoder = avcodec_find_encoder(codec_id);
    return encoder && encoder->type == AVMEDIA_TYPE_VIDEO ? encoder : nullptr;
}

const AVCodec *findNamedVideoEncoder(const char *name) {
    if (!name || !*name) return nullptr;

    const AVCodec *encoder = avcodec_find_encoder_by_name(name);
    return encoder && encoder->type == AVMEDIA_TYPE_VIDEO ? encoder : nullptr;
}

const AVCodec *findSoftwareVideoEncoderForCodecId(enum AVCodecID codec_id) {
    if (const AVCodec *encoder = findNamedVideoEncoder(softwareVideoEncoderName(codec_id))) {
        return encoder;
    }

    const AVCodec *encoder = findVideoEncoderById(codec_id);
    if (encoder && !FFmpegCommonUtils::isHardwareVideoEncoder(encoder)) return encoder;
    return nullptr;
}

} // namespace

void FFmpegCommonUtils::installPlatformLogBridge() {
    static std::once_flag install_once;
    std::call_once(install_once, []() {
        av_log_set_callback(ffmpegPlatformLogCallback);
    });
}

void FFmpegCommonUtils::logMessage(int ffmpeg_log_level, const char *format, ...) {
    installPlatformLogBridge();

    va_list args;
    va_start(args, format);
    const std::string message = formatLogMessage(format, args);
    va_end(args);

    dispatchPlatformLog(ffmpeg_log_level, message.c_str());
}

void FFmpegCommonUtils::printError(const char *prefix, int errnum) {
    char error_buffer[128];
    av_strerror(errnum, error_buffer, sizeof(error_buffer));
    AWESOME_FF_LOGE("%s: %s", prefix ? prefix : "FFmpeg error", error_buffer);
}

void FFmpegCommonUtils::printMissingAacEncoderHint() {
#if defined(__APPLE__)
    AWESOME_FF_LOGE("No AAC encoder available. Tried aac_at, aac, and codec id AAC.");
#else
    AWESOME_FF_LOGE("No AAC encoder available. Tried aac and codec id AAC.");
#endif
}

int64_t FFmpegCommonUtils::getDurationInUsSafe(AVFormatContext *context) {
    if (!context) return 0;
    if (context->duration > 0 && context->duration != AV_NOPTS_VALUE) {
        return context->duration;
    }

    int64_t max_duration = 0;
    for (unsigned int index = 0; index < context->nb_streams; ++index) {
        AVStream *stream = context->streams[index];
        if (stream->duration > 0 && stream->duration != AV_NOPTS_VALUE) {
            const int64_t duration = av_rescale_q(stream->duration, stream->time_base, AV_TIME_BASE_Q);
            if (duration > max_duration) max_duration = duration;
        }
    }
    return max_duration;
}

const char *FFmpegCommonUtils::codecNameOrUnknown(const AVCodec *codec) {
    return codec && codec->name ? codec->name : "unknown";
}

int FFmpegCommonUtils::isVideoToolboxEncoder(const AVCodec *encoder) {
    return encoder && encoder->name && strstr(encoder->name, "videotoolbox") != nullptr;
}

int FFmpegCommonUtils::isMediaCodecEncoder(const AVCodec *encoder) {
    return encoder && encoder->name && strstr(encoder->name, "mediacodec") != nullptr;
}

int FFmpegCommonUtils::isHardwareVideoEncoder(const AVCodec *encoder) {
    return isVideoToolboxEncoder(encoder) || isMediaCodecEncoder(encoder);
}

int FFmpegCommonUtils::shouldAttemptVideoEncoderRecovery(
    const AVCodecContext *encoder_context,
    int error_code
) {
    if (!encoder_context || !encoder_context->codec) return 0;
    if (!isHardwareVideoEncoder(encoder_context->codec)) return 0;

    switch (error_code) {
        case AVERROR_EXTERNAL:
        case AVERROR(EINVAL):
        case AVERROR(EIO):
        case AVERROR_INVALIDDATA:
        case AVERROR(ENODEV):
            return 1;
        default:
            return 0;
    }
}

void FFmpegCommonUtils::sanitizeVideoEncoderContextDefaults(
    AVCodecContext *encoder_context,
    const AVCodec *encoder
) {
    if (!encoder_context || !encoder) return;

    if (isVideoToolboxEncoder(encoder)) {
        encoder_context->flags &= ~AV_CODEC_FLAG_QSCALE;
        encoder_context->global_quality = 0;
        encoder_context->qmin = -1;
        encoder_context->qmax = -1;
        encoder_context->max_qdiff = -1;
    }
}

const AVCodec *FFmpegCommonUtils::findHardwareVideoEncoder(enum AVCodecID codec_id) {
    if (!canPreferVideoToolboxEncoder() && !canPreferMediaCodecEncoder()) return nullptr;
    return findNamedVideoEncoder(hardwareVideoEncoderName(codec_id));
}

const AVCodec *FFmpegCommonUtils::findSoftwareVideoEncoder(enum AVCodecID codec_id) {
    return findSoftwareVideoEncoderForCodecId(codec_id);
}

const AVCodec *FFmpegCommonUtils::findPreferredVideoEncoder(enum AVCodecID codec_id) {
    if (const AVCodec *encoder = findHardwareVideoEncoder(codec_id)) {
        return encoder;
    }

    if (const AVCodec *encoder = findSoftwareVideoEncoder(codec_id)) {
        return encoder;
    }

    if (const AVCodec *encoder = findVideoEncoderById(codec_id)) {
        return encoder;
    }
    return nullptr;
}

const AVCodec *FFmpegCommonUtils::findFallbackSoftwareVideoEncoder(enum AVCodecID codec_id) {
    if (const AVCodec *encoder = findSoftwareVideoEncoder(codec_id)) {
        return encoder;
    }

    for (size_t index = 0; index < sizeof(kVideoEncoderFallbacks) / sizeof(kVideoEncoderFallbacks[0]); ++index) {
        if (kVideoEncoderFallbacks[index] == codec_id) continue;

        if (const AVCodec *encoder = findSoftwareVideoEncoder(kVideoEncoderFallbacks[index])) {
            return encoder;
        }
    }
    return nullptr;
}

const AVCodec *FFmpegCommonUtils::findFallbackVideoEncoder(enum AVCodecID codec_id) {
    if (const AVCodec *encoder = findPreferredVideoEncoder(codec_id)) {
        return encoder;
    }

    for (size_t index = 0; index < sizeof(kVideoEncoderFallbacks) / sizeof(kVideoEncoderFallbacks[0]); ++index) {
        if (kVideoEncoderFallbacks[index] == codec_id) continue;

        if (const AVCodec *encoder = findPreferredVideoEncoder(kVideoEncoderFallbacks[index])) {
            return encoder;
        }
    }
    return nullptr;
}

const AVCodec *FFmpegCommonUtils::findPreferredAudioEncoder(enum AVCodecID codec_id) {
    if (codec_id == AV_CODEC_ID_AAC) {
#if defined(__APPLE__)
        if (const AVCodec *encoder = avcodec_find_encoder_by_name("aac_at")) {
            return encoder;
        }
#endif
        if (const AVCodec *encoder = avcodec_find_encoder_by_name("aac")) {
            return encoder;
        }
    }

    const AVCodec *encoder = avcodec_find_encoder(codec_id);
    return encoder && encoder->type == AVMEDIA_TYPE_AUDIO ? encoder : nullptr;
}

int FFmpegCommonUtils::reopenVideoEncoderWithFallback(
    const char *encoder_role,
    const AVCodec **encoder,
    const AVCodec *fallback_encoder,
    int open_ret,
    const std::function<int(const AVCodec *)> &configure_encoder,
    const std::function<int(const AVCodec *)> &open_encoder
) {
    const char *role = encoder_role && encoder_role[0] ? encoder_role : "video encoder";

    if (open_ret >= 0) {
        if (encoder && *encoder) {
            AWESOME_FF_LOGI("Using %s: %s", role, codecNameOrUnknown(*encoder));
        }
        return open_ret;
    }
    if (!encoder || !*encoder || !fallback_encoder || fallback_encoder == *encoder) return open_ret;
    if (!configure_encoder || !open_encoder) return open_ret;
    if (!isHardwareVideoEncoder(*encoder)) return open_ret;

    char error_buffer[128];
    if (av_strerror(open_ret, error_buffer, sizeof(error_buffer)) < 0) {
        snprintf(error_buffer, sizeof(error_buffer), "error %d", open_ret);
    }

    AWESOME_FF_LOGW(
        "Failed to open hardware %s: %s (%s). Retrying with software encoder: %s",
        role,
        codecNameOrUnknown(*encoder),
        error_buffer,
        codecNameOrUnknown(fallback_encoder)
    );

    int ret = configure_encoder(fallback_encoder);
    if (ret < 0) return ret;

    *encoder = fallback_encoder;
    const int ret_after_fallback = open_encoder(*encoder);
    if (ret_after_fallback >= 0) {
        AWESOME_FF_LOGI("Using %s: %s", role, codecNameOrUnknown(*encoder));
    }
    return ret_after_fallback;
}

enum AVPixelFormat FFmpegCommonUtils::chooseVideoPixelFormat(
    const AVCodec *encoder,
    enum AVPixelFormat preferred
) {
    if (!encoder || !encoder->pix_fmts) {
        return preferred != AV_PIX_FMT_NONE ? preferred : AV_PIX_FMT_YUV420P;
    }

    if (isVideoToolboxEncoder(encoder)) {
        for (const enum AVPixelFormat *format = encoder->pix_fmts; *format != AV_PIX_FMT_NONE; ++format) {
            if (*format == AV_PIX_FMT_NV12) return *format;
        }
    }

    if (isMediaCodecEncoder(encoder)) {
        const enum AVPixelFormat mediacodec_preferred_formats[] = {
            AV_PIX_FMT_NV12,
            AV_PIX_FMT_NV21,
            AV_PIX_FMT_YUV420P,
        };
        for (enum AVPixelFormat candidate : mediacodec_preferred_formats) {
            for (const enum AVPixelFormat *format = encoder->pix_fmts;
                 *format != AV_PIX_FMT_NONE;
                 ++format) {
                if (*format == candidate) return *format;
            }
        }
    }

    if (preferred != AV_PIX_FMT_NONE) {
        for (const enum AVPixelFormat *format = encoder->pix_fmts; *format != AV_PIX_FMT_NONE; ++format) {
            if (*format == preferred) return preferred;
        }
    }

    for (const enum AVPixelFormat *format = encoder->pix_fmts; *format != AV_PIX_FMT_NONE; ++format) {
        if (*format == AV_PIX_FMT_YUV420P) return *format;
    }
    return encoder->pix_fmts[0];
}

int FFmpegCommonUtils::chooseSampleRate(const AVCodec *encoder, int preferred) {
    if (!encoder || !encoder->supported_samplerates) {
        return preferred > 0 ? preferred : 44100;
    }

    int selected = encoder->supported_samplerates[0];
    if (preferred > 0) {
        int best_distance = INT_MAX;
        for (const int *rate = encoder->supported_samplerates; *rate != 0; ++rate) {
            const int distance = abs(*rate - preferred);
            if (distance < best_distance) {
                best_distance = distance;
                selected = *rate;
            }
            if (*rate == preferred) return preferred;
        }
    }
    return selected > 0 ? selected : 44100;
}

enum AVSampleFormat FFmpegCommonUtils::chooseSampleFormat(
    const AVCodec *encoder,
    enum AVSampleFormat preferred
) {
    if (!encoder || !encoder->sample_fmts) {
        return preferred != AV_SAMPLE_FMT_NONE ? preferred : AV_SAMPLE_FMT_FLTP;
    }

    if (preferred != AV_SAMPLE_FMT_NONE) {
        for (const enum AVSampleFormat *format = encoder->sample_fmts; *format != AV_SAMPLE_FMT_NONE; ++format) {
            if (*format == preferred) return preferred;
        }
    }

    for (const enum AVSampleFormat *format = encoder->sample_fmts; *format != AV_SAMPLE_FMT_NONE; ++format) {
        if (*format == AV_SAMPLE_FMT_FLTP) return *format;
    }
    return encoder->sample_fmts[0];
}

int FFmpegCommonUtils::chooseChannelLayout(
    const AVCodec *encoder,
    const AVChannelLayout *preferred,
    AVChannelLayout *selected
) {
    if (!selected) return AVERROR(EINVAL);

    const int preferred_channels = preferred && preferred->nb_channels > 0 ? preferred->nb_channels : 0;
    const AVChannelLayout *fallback_layout = nullptr;
    const AVChannelLayout *same_channels_layout = nullptr;

    if (encoder && encoder->ch_layouts) {
        for (const AVChannelLayout *layout = encoder->ch_layouts; layout->nb_channels > 0; ++layout) {
            if (!fallback_layout) fallback_layout = layout;
            if (preferred && av_channel_layout_compare(layout, preferred) == 0) {
                return av_channel_layout_copy(selected, layout);
            }
            if (!same_channels_layout && preferred_channels > 0 && layout->nb_channels == preferred_channels) {
                same_channels_layout = layout;
            }
        }

        if (same_channels_layout) {
            return av_channel_layout_copy(selected, same_channels_layout);
        }

        if (fallback_layout) {
            return av_channel_layout_copy(selected, fallback_layout);
        }
    }

    if (preferred_channels > 0) {
        return av_channel_layout_copy(selected, preferred);
    }

    av_channel_layout_default(selected, 2);
    return 0;
}

int FFmpegCommonUtils::copyDisplayMatrixSideData(AVStream *input_stream, AVStream *output_stream) {
    if (!input_stream || !input_stream->codecpar || !output_stream || !output_stream->codecpar) {
        return AVERROR(EINVAL);
    }

    const AVPacketSideData *display_matrix = av_packet_side_data_get(
        input_stream->codecpar->coded_side_data,
        input_stream->codecpar->nb_coded_side_data,
        AV_PKT_DATA_DISPLAYMATRIX
    );
    if (!display_matrix || !display_matrix->data || display_matrix->size == 0) return 0;

    AVPacketSideData *copied = av_packet_side_data_new(
        &output_stream->codecpar->coded_side_data,
        &output_stream->codecpar->nb_coded_side_data,
        AV_PKT_DATA_DISPLAYMATRIX,
        display_matrix->size,
        0
    );
    if (!copied || !copied->data) return AVERROR(ENOMEM);

    memcpy(copied->data, display_matrix->data, display_matrix->size);
    return 0;
}

int FFmpegCommonUtils::normalizeVideoDimension(int value) {
    if (value <= 0) return 0;
    if ((value & 1) != 0) --value;
    return value < 2 ? 2 : value;
}

int FFmpegCommonUtils::resolveOutputVideoSize(
    int source_width,
    int source_height,
    int requested_width,
    int requested_height,
    int *output_width,
    int *output_height
) {
    if (!output_width || !output_height) return AVERROR(EINVAL);
    if (source_width <= 0 || source_height <= 0) return AVERROR(EINVAL);

    int width = requested_width;
    int height = requested_height;

    if (width <= 0 && height <= 0) {
        width = source_width;
        height = source_height;
    } else if (width <= 0) {
        width = static_cast<int>(av_rescale(height, source_width, source_height));
    } else if (height <= 0) {
        height = static_cast<int>(av_rescale(width, source_height, source_width));
    }

    width = normalizeVideoDimension(width);
    height = normalizeVideoDimension(height);
    if (width <= 0 || height <= 0) return AVERROR(EINVAL);

    *output_width = width;
    *output_height = height;
    return 0;
}

int FFmpegCommonUtils::parseH264Level(const std::string &level) {
    if (level.empty()) return 31;

    char *end = nullptr;
    const long raw = strtol(level.c_str(), &end, 10);
    if (end && *end == '\0' && raw > 0) {
        return static_cast<int>(raw);
    }

    const char *dot = strchr(level.c_str(), '.');
    if (!dot) return 0;

    const std::string major(level.c_str(), dot - level.c_str());
    const std::string minor(dot + 1);
    if (major.empty() || minor.empty()) return 0;

    end = nullptr;
    const long major_value = strtol(major.c_str(), &end, 10);
    if (!end || *end != '\0' || major_value < 0) return 0;

    end = nullptr;
    const long minor_value = strtol(minor.c_str(), &end, 10);
    if (!end || *end != '\0' || minor_value < 0) return 0;

    return static_cast<int>(major_value * 10 + minor_value);
}
