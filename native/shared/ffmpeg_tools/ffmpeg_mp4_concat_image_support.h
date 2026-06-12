#ifndef AWESOME_FFMPEG_MP4_CONCAT_IMAGE_SUPPORT_H
#define AWESOME_FFMPEG_MP4_CONCAT_IMAGE_SUPPORT_H

#include "ffmpeg_hls_common.h"

#include <functional>

struct FFmpegMp4ConcatImageRuntimeCallbacks {
    std::function<bool()> is_cancelled;
    std::function<int()> wait_if_paused;
    std::function<int(const char *reason)> rebuild_video_encoder_after_pause;
    std::function<int(AVFrame *frame, int64_t output_pts)> send_prepared_video_frame;
    std::function<int(int flush_last_frame)> drain_audio_fifo;
};

class FFmpegMp4ConcatImageSupport {
public:
    static int initializeConcatImageFrame(
        const char *image_path,
        AVFormatContext *input_format,
        int video_input_index,
        AVCodecContext *video_enc_ctx,
        SwsContext **sws_ctx,
        const char *rotation_log_context,
        AVFrame **concat_image_frame_out
    );

    static int generateConcatImageVideoSegment(
        AVCodecContext **video_enc_ctx,
        AVFrame **video_enc_frame,
        AVFrame **concat_image_frame,
        int64_t concat_image_duration_us,
        int64_t start_time_us,
        int64_t *next_video_pts,
        const FFmpegMp4ConcatImageRuntimeCallbacks &callbacks
    );

    static int appendSilentAudioSegment(
        AVCodecContext *audio_enc_ctx,
        AVAudioFifo *audio_fifo,
        int64_t duration_us,
        int64_t *audio_next_pts,
        const FFmpegMp4ConcatImageRuntimeCallbacks &callbacks
    );

    static int64_t currentVideoOutputTimeUs(const AVCodecContext *video_enc_ctx, int64_t next_video_pts);
    static int64_t currentAudioOutputTimeUs(
        const AVCodecContext *audio_enc_ctx,
        AVAudioFifo *audio_fifo,
        int64_t audio_next_pts
    );
};

#endif /* AWESOME_FFMPEG_MP4_CONCAT_IMAGE_SUPPORT_H */
