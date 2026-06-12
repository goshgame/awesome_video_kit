#include "ffmpeg_mp4_watermark.h"

#include "ffmpeg_mp4_watermark_transcoder.h"

int transcode_file_to_mp4_with_watermark(
    const char *input_path,
    const char *output_path,
    const FFmpegMp4TranscodeConfig *config,
    const FFmpegWatermarkConfig *watermark,
    void (*progress_cb)(int percentage, void *user_data),
    void *user_data,
    volatile int *cancel_flag,
    volatile int *pause_flag
) {
    FFmpegMp4WatermarkTranscoder transcoder(
        input_path,
        output_path,
        config,
        watermark,
        nullptr,
        progress_cb,
        user_data,
        cancel_flag,
        pause_flag
    );
    return transcoder.transcode();
}

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
) {
    FFmpegMp4WatermarkTranscoder transcoder(
        input_path,
        output_path,
        config,
        watermark,
        concat_image,
        progress_cb,
        user_data,
        cancel_flag,
        pause_flag
    );
    return transcoder.transcode();
}
