#ifndef AWESOME_FFMPEG_MP4_TRANSCODER_BASE_H
#define AWESOME_FFMPEG_MP4_TRANSCODER_BASE_H

#include "ffmpeg_hls_common.h"
#include "ffmpeg_hls_progress.h"
#include "ffmpeg_mp4.h"

#include <vector>

// 本地 MP4 转码公共基类。
// 统一收敛普通转码、水印转码、图片拼接转码三条链路共用的配置解析、
// 输入打开、基础编解码上下文、时间戳归一化、写头/写包和共享资源清理逻辑。
class FFmpegMp4TranscoderBase {
protected:
    FFmpegMp4TranscoderBase(
        const char *input_path,
        const char *output_path,
        const FFmpegMp4TranscodeConfig *config,
        void (*progress_cb)(int percentage, void *user_data),
        void *user_data,
        volatile int *cancel_flag,
        volatile int *pause_flag
    );
    FFmpegMp4TranscoderBase(
        const char *input_path,
        const char *output_path,
        void (*progress_cb)(int percentage, void *user_data),
        void *user_data,
        volatile int *cancel_flag,
        volatile int *pause_flag
    );
    virtual ~FFmpegMp4TranscoderBase() = default;

    void cleanupSharedResources();
    bool isCancelled() const;
    bool isPaused() const;
    int waitIfPaused();
    int64_t normalizePts(
        int64_t pts,
        AVRational source_time_base,
        AVRational destination_time_base,
        int64_t output_offset_pts = 0
    ) const;
    int openInputFileAndInitDecoders();
    int initializeVideoDecoder();
    int initializeAudioDecoder();
    int writeHeader();
    int writeEncodedPackets(AVCodecContext *encoder_context, int output_stream_index);
    int initializeAudioCopyStreamWithOptionalBsf(AVStream *output_stream);
    int processAudioCopyPacketWithOptionalBsf(AVPacket *packet);
    int flushAudioCopyPacketBsf();
    int writeAudioCopyPacketToMuxer(AVPacket *packet, AVRational source_time_base);
    int normalizeInputVideoPacketForDecoder(AVPacket *packet) const;
    bool hasForcedVideoFrameRate() const;
    AVRational resolveVideoFrameRate(AVFormatContext *format_context, AVStream *input_stream) const;
    int videoGopSize(AVRational frame_rate) const;
    int cacheFrameReference(AVFrame **destination, const AVFrame *source) const;
    int sendVideoFrameWithRecovery(AVFrame *frame, int64_t absolute_pts, const char *operation);
    void prepareVideoFrameForEncoding(AVFrame *frame, int64_t absolute_pts);
    void markVideoEncoderRebuilt();
    int updateVideoOutputStreamAfterEncoderChange();
    int rebuildVideoEncoderAfterPauseIfNeeded(const char *reason);
    bool shouldRebuildHardwareVideoEncoderAfterPause() const;
    void logOutputFileProbeSummary() const;
    virtual int rebuildVideoEncoderForResume() = 0;

    std::string input_path_;
    std::string output_path_;
    std::string video_preset_;
    std::string video_profile_;
    std::string video_level_;
    int video_crf_;
    int audio_bitrate_;
    int faststart_;
    int frame_rate_;
    int scale_width_;
    int scale_height_;
    void (*progress_cb_)(int percentage, void *user_data);
    void *user_data_;
    volatile int *cancel_flag_;
    volatile int *pause_flag_;

    AVFormatContext *input_format_;
    AVFormatContext *output_format_;
    AVCodecContext *video_dec_ctx_;
    AVCodecContext *video_enc_ctx_;
    AVCodecContext *audio_dec_ctx_;
    AVCodecContext *audio_enc_ctx_;
    SwsContext *sws_ctx_;
    SwrContext *swr_ctx_;
    AVAudioFifo *audio_fifo_;
    AVFrame *video_dec_frame_;
    AVFrame *audio_dec_frame_;
    AVPacket *enc_packet_;
    AVBSFContext *audio_copy_bsf_ctx_;
    int video_input_index_;
    int video_output_index_;
    int audio_input_index_;
    int audio_output_index_;
    int64_t audio_next_pts_;
    int64_t timeline_start_us_;
    int64_t video_encoder_input_origin_pts_;
    int64_t video_encoder_output_offset_pts_;
    int64_t video_last_written_pts_;
    int64_t video_last_written_dts_;
    int video_last_write_packet_count_;
    int video_debug_packets_logged_;
    bool audio_copy_mode_;
    bool video_force_keyframe_on_next_frame_;
    bool video_packets_written_;
    bool output_header_written_;
    bool video_muxer_reopen_required_;
    bool video_pending_extradata_refresh_;
    bool video_attach_extradata_to_next_packet_;
    bool video_drop_until_sync_packet_;
    bool video_require_fresh_extradata_;
    bool video_current_extradata_observed_;
    bool pause_observed_;
    bool video_rebuild_after_pause_pending_;
    std::vector<uint8_t> video_extradata_snapshot_;
    FFmpegProgressTracker progress_;
};

#endif /* AWESOME_FFMPEG_MP4_TRANSCODER_BASE_H */
