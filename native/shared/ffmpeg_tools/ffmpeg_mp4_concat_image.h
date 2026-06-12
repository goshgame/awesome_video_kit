#ifndef AWESOME_FFMPEG_MP4_CONCAT_IMAGE_H
#define AWESOME_FFMPEG_MP4_CONCAT_IMAGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FFmpegConcatImagePositionHead = 0,
    FFmpegConcatImagePositionTail = 1,
} FFmpegConcatImagePosition;

typedef struct {
    const char *image_path;
    int64_t image_duration_us;
    FFmpegConcatImagePosition position;
} FFmpegMp4ConcatImageConfig;

// 本地视频转 MP4，并在头部或尾部拼接一张静态图片。
// 支持 PNG/JPG/JPEG 等单帧图片输入。
// 图片阶段的视频由静态图片重复生成，直接复用原视频输出轨的尺寸/帧率；
// 音频使用静音补齐；原视频音频保持原样转码。
int transcode_file_to_mp4_with_concat_image(
    const char *input_path,
    const char *output_path,
    const FFmpegMp4ConcatImageConfig *concat_image,
    void (*progress_cb)(int percentage, void *user_data),
    void *user_data,
    volatile int *cancel_flag,
    volatile int *pause_flag
);

#ifdef __cplusplus
}
#endif

#endif /* AWESOME_FFMPEG_MP4_CONCAT_IMAGE_H */
