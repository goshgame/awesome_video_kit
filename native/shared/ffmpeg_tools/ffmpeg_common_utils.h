#ifndef AWESOME_FFMPEG_COMMON_UTILS_H
#define AWESOME_FFMPEG_COMMON_UTILS_H

#include "ffmpeg_hls_common.h"

#include <functional>

// FFmpeg 公共辅助工具。
// 主要承接多条转码链路都要复用的基础能力，避免 HLS、水印、本地 MP4 转码各自维护重复逻辑。
class FFmpegCommonUtils {
public:
    // 初始化平台日志桥接，把业务日志和 FFmpeg av_log 统一转发到平台原生日志系统。
    static void installPlatformLogBridge();
    // 输出统一平台日志；Android 走 logcat，iOS 走 os_log，其他平台回退 stderr。
    static void logMessage(int ffmpeg_log_level, const char *format, ...);

    // 错误与诊断辅助。
    // 输出 FFmpeg 错误码对应的可读错误信息。
    static void printError(const char *prefix, int errnum);
    // 输出 AAC 编码器缺失的统一提示信息。
    static void printMissingAacEncoderHint();
    // 安全获取媒体总时长，format 未提供 duration 时回退到 stream 时长。
    static int64_t getDurationInUsSafe(AVFormatContext *context);

    // 编码器识别与选择。
    // 返回编码器名称，空指针时返回 "unknown"。
    static const char *codecNameOrUnknown(const AVCodec *codec);
    // 判断是否为 iOS/macOS VideoToolbox 硬件视频编码器。
    static int isVideoToolboxEncoder(const AVCodec *encoder);
    // 判断是否为 Android MediaCodec 硬件视频编码器。
    static int isMediaCodecEncoder(const AVCodec *encoder);
    // 判断是否为当前支持识别的硬件视频编码器。
    static int isHardwareVideoEncoder(const AVCodec *encoder);
    // 判断当前硬件视频编码错误是否值得尝试重建编码器恢复。
    static int shouldAttemptVideoEncoderRecovery(const AVCodecContext *encoder_context, int error_code);
    // 清理硬件编码器不支持的通用编码字段，避免默认值被错误透传到平台编码器。
    static void sanitizeVideoEncoderContextDefaults(AVCodecContext *encoder_context, const AVCodec *encoder);

    // 按 codec id 查找平台优先的硬件视频编码器。
    static const AVCodec *findHardwareVideoEncoder(enum AVCodecID codec_id);
    // 按 codec id 查找软件视频编码器。
    static const AVCodec *findSoftwareVideoEncoder(enum AVCodecID codec_id);
    // 优先返回硬件视频编码器，找不到时回退到软件编码器。
    static const AVCodec *findPreferredVideoEncoder(enum AVCodecID codec_id);
    // 当前 codec id 无可用软件编码器时，尝试其他常见视频编码器的软件实现。
    static const AVCodec *findFallbackSoftwareVideoEncoder(enum AVCodecID codec_id);
    // 当前 codec id 无可用编码器时，尝试其他常见视频编码器，仍然保持硬优先软回退。
    static const AVCodec *findFallbackVideoEncoder(enum AVCodecID codec_id);
    // 查找音频编码器，内部会按平台优先级选择更合适的 AAC 编码器实现。
    static const AVCodec *findPreferredAudioEncoder(enum AVCodecID codec_id);
    // 当硬件视频编码器打开失败时，按调用方提供的软编码器候选重试一次。
    static int reopenVideoEncoderWithFallback(
        const char *encoder_role,
        const AVCodec **encoder,
        const AVCodec *fallback_encoder,
        int open_ret,
        const std::function<int(const AVCodec *)> &configure_encoder,
        const std::function<int(const AVCodec *)> &open_encoder
    );

    // 编码参数协商。
    // 从编码器支持的像素格式中选择最合适的输出格式，优先使用 preferred。
    static enum AVPixelFormat chooseVideoPixelFormat(
        const AVCodec *encoder,
        enum AVPixelFormat preferred = AV_PIX_FMT_NONE
    );
    // 从编码器支持的采样率中选择最接近 preferred 的值。
    static int chooseSampleRate(const AVCodec *encoder, int preferred);
    // 从编码器支持的采样格式中选择最合适的格式，优先使用 preferred。
    static enum AVSampleFormat chooseSampleFormat(
        const AVCodec *encoder,
        enum AVSampleFormat preferred
    );
    // 根据编码器能力选择声道布局，优先保持与输入一致。
    static int chooseChannelLayout(
        const AVCodec *encoder,
        const AVChannelLayout *preferred,
        AVChannelLayout *selected
    );

    // 视频尺寸和元数据处理。
    // 复制视频流的显示矩阵信息，保留原视频旋转/方向元数据。
    static int copyDisplayMatrixSideData(AVStream *input_stream, AVStream *output_stream);
    // 将视频宽高规范化为有效偶数，避免编码器因奇数尺寸失败。
    static int normalizeVideoDimension(int value);
    // 根据源尺寸和目标宽高请求解析最终输出尺寸，并保证结果合法可编码。
    static int resolveOutputVideoSize(
        int source_width,
        int source_height,
        int requested_width,
        int requested_height,
        int *output_width,
        int *output_height
    );

    // 配置解析辅助。
    // 解析 H.264 level 字符串，支持 "31" 和 "3.1" 两种格式。
    static int parseH264Level(const std::string &level);
};

#define AWESOME_FF_LOGD(...) FFmpegCommonUtils::logMessage(AV_LOG_DEBUG, __VA_ARGS__)
#define AWESOME_FF_LOGI(...) FFmpegCommonUtils::logMessage(AV_LOG_INFO, __VA_ARGS__)
#define AWESOME_FF_LOGW(...) FFmpegCommonUtils::logMessage(AV_LOG_WARNING, __VA_ARGS__)
#define AWESOME_FF_LOGE(...) FFmpegCommonUtils::logMessage(AV_LOG_ERROR, __VA_ARGS__)

#endif /* AWESOME_FFMPEG_COMMON_UTILS_H */
