#ifndef AWESOME_FFMPEG_MP4_WATERMARK_H
#define AWESOME_FFMPEG_MP4_WATERMARK_H

#include "ffmpeg_mp4_concat_image.h"
#include "ffmpeg_hls.h"
#include "ffmpeg_mp4.h"

#ifdef __cplusplus
extern "C" {
#endif

// 本地视频转 MP4 并叠加图片水印。
// 这是给 Objective-C 管理层调用的 C 接口，内部由专用本地水印转码器实现。
int transcode_file_to_mp4_with_watermark(
    const char *input_path,
    const char *output_path,
    const FFmpegMp4TranscodeConfig *config,
    const FFmpegWatermarkConfig *watermark,
    void (*progress_cb)(int percentage, void *user_data),
    void *user_data,
    volatile int *cancel_flag,
    volatile int *pause_flag
);

// 本地视频转 MP4，先叠加图片水印，再在头部或尾部拼接一段静态图片。
// 整个过程只进行一次视频编码。
int transcode_file_to_mp4_with_watermark_and_concat_image(
    const char *input_path,
    const char *output_path,
    const FFmpegMp4TranscodeConfig *config,
    const FFmpegWatermarkConfig *watermark,
    const FFmpegMp4ConcatImageConfig *concat_image,
    void (*progress_cb)(int percentage, void *user_data),
    void *user_data,
    volatile int *cancel_flag,
    volatile int *pause_flag
);

#ifdef __cplusplus
}
#endif

#endif /* AWESOME_FFMPEG_MP4_WATERMARK_H */
