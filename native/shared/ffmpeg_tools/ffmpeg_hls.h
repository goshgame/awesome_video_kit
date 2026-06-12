//
//  ffmpeg_hls.h
//  M3U8Demo
//
//  Created by dev on 2026/1/12.
//

#ifndef FFMPEG_HLS_H
#define FFMPEG_HLS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FFmpegWatermarkPositionTopLeft = 0,
    FFmpegWatermarkPositionTopRight = 1,
    FFmpegWatermarkPositionBottomLeft = 2,
    FFmpegWatermarkPositionBottomRight = 3,
    FFmpegWatermarkPositionCenter = 4,
    // 按 5 秒为一个周期交替切换位置：
    // 0-5 秒左上角，5-10 秒右下角，10-15 秒左上角，循环往复。
    FFmpegWatermarkPositionAlternatingTopLeftBottomRight = 5,
} FFmpegWatermarkPosition;

typedef struct {
    // 本地图片路径；支持 PNG/JPG/JPEG，若需要透明度建议使用 PNG。
    // 内部会先解码为 AVFrame，再作为 overlay 第二路输入。
    const char *image_path;
    FFmpegWatermarkPosition position;
    // 边距（像素）。<= 0 时内部按 20 处理。
    int margin_x;
    int margin_y;
    // 目标尺寸（像素）。width/height 同时 <= 0 时按视频分辨率自适应缩放，
    // 且不会把原图放大；仅设置单边时，另一边会在 scale 里用 -1 保持比例。
    int width;
    int height;
} FFmpegWatermarkConfig;

/**
 * 下载 m3u8 到 mp4，带进度回调
 * @param m3u8_url 输入 m3u8 URL
 * @param output_path 输出 mp4 文件路径
 * @param progress_cb 进度回调函数，参数是百分比 0~100，user_data 原样传入
 * @param user_data 传递给回调的用户数据指针
 * @param cancel_flag 用于外部控制取消下载
 * @return 0 成功，负数错误码
 */
int download_m3u8_to_mp4(
    const char *m3u8_url,
    const char *output_path,
    void (*progress_cb)(int percentage, void *user_data),
    void *user_data,
    volatile int *cancel_flag   // 新增取消标志指针
);

/**
 * 下载 m3u8 到 mp4，并可使用 FFmpeg overlay 滤镜叠加图片水印。
 * 说明：
 * 1) watermark 为 NULL 或 image_path 为空时，退化为原始 remux 下载逻辑；
 * 2) 传入 watermark 时，视频流会走 decode -> overlay filter -> encode，音频保持拷贝；
 * 3) 依赖 FFmpeg 构建中包含 libavfilter/libswscale（以及常见图片解码能力）。
 */
int download_m3u8_to_mp4_with_watermark(
    const char *m3u8_url,
    const char *output_path,
    const FFmpegWatermarkConfig *watermark,
    void (*progress_cb)(int percentage, void *user_data),
    void *user_data,
    volatile int *cancel_flag
);

#ifdef __cplusplus
}
#endif

#endif /* FFMPEG_HLS_H */
