#ifndef AWESOME_FFMPEG_VIDEO_FRAME_EXTRACTOR_H
#define AWESOME_FFMPEG_VIDEO_FRAME_EXTRACTOR_H

#ifdef __cplusplus
extern "C" {
#endif

int capture_video_frames_to_jpegs_cpp(
    const char *input_path,
    const double *times_seconds,
    const char *const *output_paths,
    int count,
    int output_width,
    int output_height,
    int fast_mode,
    volatile int *cancel_flag
);

#ifdef __cplusplus
}
#endif

#endif /* AWESOME_FFMPEG_VIDEO_FRAME_EXTRACTOR_H */
