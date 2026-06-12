//
//  ffmpeg_mp4.cpp
//  AwesomeVideoKitSDK
//
//  Created by dev on 2026/3/20.
//

#include "ffmpeg_mp4.h"

#include "ffmpeg_mp4_transcoder.h"

extern "C" {

int transcode_file_to_mp4(
    const char *input_path,
    const char *output_path,
    const FFmpegMp4TranscodeConfig *config,
    void (*progress_cb)(int percentage, void *user_data),
    void *user_data,
    volatile int *cancel_flag,
    volatile int *pause_flag
) {
    FFmpegMp4Transcoder transcoder(
        input_path,
        output_path,
        config,
        progress_cb,
        user_data,
        cancel_flag,
        pause_flag
    );
    return transcoder.transcode();
}

int transcode_file_with_separate_audio_to_mp4(
    const char *visual_input_path,
    const char *audio_input_path,
    const char *output_path,
    const FFmpegMp4TranscodeConfig *config,
    void (*progress_cb)(int percentage, void *user_data),
    void *user_data,
    volatile int *cancel_flag,
    volatile int *pause_flag
) {
    return transcode_file_with_separate_audio_to_mp4_cpp(
        visual_input_path,
        audio_input_path,
        output_path,
        config,
        progress_cb,
        user_data,
        cancel_flag,
        pause_flag
    );
}

int extract_audio_stream_from_media(
    const char *input_path,
    const char *output_path,
    void (*progress_cb)(int percentage, void *user_data),
    void *user_data,
    volatile int *cancel_flag,
    volatile int *pause_flag
) {
    return extract_audio_stream_from_media_cpp(
        input_path,
        output_path,
        progress_cb,
        user_data,
        cancel_flag,
        pause_flag
    );
}

} // extern "C"
