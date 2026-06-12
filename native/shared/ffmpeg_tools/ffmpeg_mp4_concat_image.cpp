#include "ffmpeg_mp4_concat_image.h"

#include "ffmpeg_mp4_concat_image_transcoder.h"

int transcode_file_to_mp4_with_concat_image(
    const char *input_path,
    const char *output_path,
    const FFmpegMp4ConcatImageConfig *concat_image,
    void (*progress_cb)(int percentage, void *user_data),
    void *user_data,
    volatile int *cancel_flag,
    volatile int *pause_flag
) {
    FFmpegMp4ConcatImageTranscoder transcoder(
        input_path,
        output_path,
        concat_image,
        progress_cb,
        user_data,
        cancel_flag,
        pause_flag
    );
    return transcoder.transcode();
}
