#ifndef AWESOME_FFMPEG_MP4_CONCAT_IMAGE_TRANSCODER_H
#define AWESOME_FFMPEG_MP4_CONCAT_IMAGE_TRANSCODER_H

#include "ffmpeg_mp4_concat_image.h"
#include "ffmpeg_mp4_transcoder_base.h"

// 本地文件图片拼接专用转码器。
// 负责 decode -> encode 原视频，并在头部或尾部插入静态图片段和对应静音音频。
class FFmpegMp4ConcatImageTranscoder : public FFmpegMp4TranscoderBase {
public:
    FFmpegMp4ConcatImageTranscoder(
        const char *input_path,
        const char *output_path,
        const FFmpegMp4ConcatImageConfig *concat_image,
        void (*progress_cb)(int percentage, void *user_data),
        void *user_data,
        volatile int *cancel_flag,
        volatile int *pause_flag
    );
    ~FFmpegMp4ConcatImageTranscoder();

    int transcode();

private:
    std::string concat_image_path_;
    int64_t concat_image_duration_us_;
    FFmpegConcatImagePosition concat_image_position_;
    AVFrame *video_enc_frame_;
    AVFrame *concat_image_frame_;
    AVFrame *last_video_frame_;
    int64_t input_duration_us_;
    int64_t video_next_pts_;
    int64_t video_segment_offset_pts_;

    void cleanup();
    bool hasConcatImage() const;

    int openInput();
    int openOutput();
    int initializeVideoEncoder();
    int configureVideoEncoder(bool create_output_stream);
    int initializeAudioEncoder();
    int initializeFrames();
    int initializeConcatImageFrame();
    int transcodeLoop();

    int processVideoPacket(AVPacket *packet);
    int processAudioPacket(AVPacket *packet);
    int processDecodedVideoFrame(AVFrame *frame);
    int processDecodedAudioFrame(AVFrame *frame);

    int flushSourceVideoDecoder();
    int flushSourceAudioDecoder();
    int finalizeVideoEncoder();
    int finalizeAudioEncoder();
    int flushResamplerToFifo();
    int drainAudioFifo(int flush_last_frame);
    int duplicateLastVideoFrameUntil(int64_t target_pts);
    int sendPreparedVideoFrame(AVFrame *frame, int64_t output_pts);
    int generateConcatImageVideoSegment(int64_t start_time_us);
    int appendSilentAudioSegment(int64_t duration_us);
    int appendConcatImageSegmentIfNeeded();
    int prependConcatImageSegmentIfNeeded();
    int64_t currentVideoOutputTimeUs() const;
    int64_t currentAudioOutputTimeUs() const;
    int refreshVideoEncodeBuffer();
    int rebuildVideoEncoderForResume() override;
};

#endif /* AWESOME_FFMPEG_MP4_CONCAT_IMAGE_TRANSCODER_H */
