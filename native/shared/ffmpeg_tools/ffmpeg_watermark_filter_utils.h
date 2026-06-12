#ifndef AWESOME_FFMPEG_WATERMARK_FILTER_UTILS_H
#define AWESOME_FFMPEG_WATERMARK_FILTER_UTILS_H

#include "ffmpeg_hls_common.h"

// 图片水印相关的公共辅助逻辑。
// 负责水印图片解码、overlay 参数计算和滤镜图初始化，供网络/本地两套水印流程复用。
class FFmpegWatermarkFilterUtils {
public:
    // 判断当前配置是否真正启用了图片水印。
    static int isEnabled(const FFmpegWatermarkConfig *watermark);

    // 根据水印位置和边距生成 overlay 的 x/y 表达式。
    // 支持固定位置，也支持按时间动态切换的位置表达式。
    static void buildOverlayPositionExpr(
        FFmpegWatermarkPosition position,
        int margin_x,
        int margin_y,
        char *x_expr,
        size_t x_expr_size,
        char *y_expr,
        size_t y_expr_size
    );

    // 计算默认自适应水印在当前视频边长上的最大像素值。
    static int computeAutoWatermarkBound(int video_side);

    // 判断当前水印是否需要经过 scale 滤镜。
    static int shouldScaleWatermark(
        const FFmpegWatermarkConfig *watermark,
        const AVFrame *watermark_frame,
        int auto_max_width,
        int auto_max_height
    );

    // 生成水印 scale 滤镜参数，显式尺寸优先，其次走视频自适应上限。
    static void buildWatermarkScaleArgs(
        const FFmpegWatermarkConfig *watermark,
        int auto_max_width,
        int auto_max_height,
        char *scale_args,
        size_t scale_args_size
    );

    // 从单帧图片文件中解码出可供 overlay 使用的帧，支持 PNG/JPG/JPEG。
    static int decodeImageFileToFrame(
        const char *image_path,
        AVFrame **frame_out,
        AVRational *time_base_out
    );

    // 初始化主视频 + 图片水印所需的滤镜图。
    // output_width/output_height 用于本地主视频缩放，传入输入尺寸时表示不缩放主视频。
    static int initializeWatermarkFilterGraph(
        AVCodecContext *video_dec_ctx,
        AVCodecContext *video_enc_ctx,
        AVStream *video_input_stream,
        int output_width,
        int output_height,
        const FFmpegWatermarkConfig *watermark,
        AVFilterGraph **filter_graph_out,
        AVFilterContext **buffer_src_ctx_out,
        AVFilterContext **buffer_sink_ctx_out,
        AVRational *filter_time_base_out
    );

private:
    // 把图片文件完整读入内存缓冲区。
    static int readFileToBuffer(const char *path, uint8_t **data_out, size_t *size_out);
    // 判断内存缓冲区是否是 PNG 文件头。
    static int bufferIsPng(const uint8_t *data, size_t size);
    // 判断内存缓冲区是否是 JPG/JPEG 文件头。
    static int bufferIsJpeg(const uint8_t *data, size_t size);
    // 直接把 PNG 二进制数据解码成 AVFrame。
    static int decodePngDataToFrame(
        const uint8_t *data,
        size_t size,
        AVFrame **frame_out,
        AVRational *time_base_out
    );
    // 直接把 JPG/JPEG 二进制数据解码成 AVFrame。
    static int decodeJpegDataToFrame(
        const uint8_t *data,
        size_t size,
        AVFrame **frame_out,
        AVRational *time_base_out
    );
};

#endif /* AWESOME_FFMPEG_WATERMARK_FILTER_UTILS_H */
