//
//  ffmpeg_mp4_transcoder.h
//  AwesomeVideoKitSDK
//
//  Created by dev on 2026/3/20.
//

#ifndef AWESOME_FFMPEG_MP4_TRANSCODER_H
#define AWESOME_FFMPEG_MP4_TRANSCODER_H

#include "ffmpeg_mp4_transcoder_base.h"

class FFmpegMp4Transcoder : public FFmpegMp4TranscoderBase {
public:
    // 保存一次本地文件转 mp4 所需的输入参数和回调。
    FFmpegMp4Transcoder(
        const char *input_path,
        const char *output_path,
        const FFmpegMp4TranscodeConfig *config,
        void (*progress_cb)(int percentage, void *user_data),
        void *user_data,
        volatile int *cancel_flag,
        volatile int *pause_flag
    );
    // 释放解复用、编解码和音视频转换阶段占用的资源。
    ~FFmpegMp4Transcoder();

    // 执行完整转码流程，输出 H.264/AAC 的 mp4 文件。
    int transcode();

private:
    AVFrame *video_enc_frame_;
    AVFrame *last_video_frame_;
    int64_t next_video_pts_;
    int input_video_debug_packets_logged_;

    // 释放当前转码任务已申请的全部 FFmpeg 对象。
    void cleanup();

    // 打开输入文件、读取流信息并初始化进度状态。
    int openInput();
    // 创建输出容器并初始化编码器、帧缓冲和输出 IO。
    int openOutput();
    // 按 x264 风格参数和目标分辨率初始化视频编码器及输出视频流。
    int initializeVideoEncoder();
    // 复用当前配置重新打开视频编码器；首次创建时同步建立输出视频流。
    int configureVideoEncoder(bool create_output_stream);
    // 初始化 AAC 编码器、重采样器和音频 FIFO。
    int initializeAudioEncoder();
    // 当没有 AAC 编码器但输入已经是 AAC 时，直接复制音频流。
    int initializeAudioCopyStream();
    // 为视频编码和音频解码阶段申请复用帧对象。
    int initializeFrames();
    // 主循环读取输入 packet，驱动音视频解码、转换和编码。
    int transcodeLoop();

    // 处理一个输入视频包，完成解码并推进后续编码链路。
    int processVideoPacket(AVPacket *packet);
    // 处理一个输入音频包，完成解码并推进后续编码链路。
    int processAudioPacket(AVPacket *packet);
    // 直接复制输入 AAC 音频包到输出 mp4。
    int processAudioPacketCopy(AVPacket *packet);
    // 把目标帧送入编码器，并缓存最近一帧供补帧使用。
    int sendPreparedVideoFrame(AVFrame *frame, int64_t output_pts);
    // 当目标帧率高于源帧率时，复制上一帧填补中间的时间空洞。
    int duplicateLastVideoFrameUntil(int64_t target_pts);
    // 把解码后的视频帧转换为编码器目标像素格式并送入编码器。
    int processDecodedVideoFrame(AVFrame *frame);
    // 把解码后音频重采样后写入 FIFO，并尝试输出编码帧。
    int processDecodedAudioFrame(AVFrame *frame);
    // 在编码器重建后刷新复用的视频编码帧缓存。
    int refreshVideoEncodeBuffer();
    // VideoToolbox 等硬件编码器失效后，重建视频编码器并续写时间轴。
    int rebuildVideoEncoderForResume() override;

    // 刷新视频解码器和编码器中的剩余帧。
    int flushVideoPipeline();
    // 刷新音频解码器、重采样器、FIFO 和编码器中的剩余数据。
    int flushAudioPipeline();
    // 把重采样器内部缓存的尾部采样刷入音频 FIFO。
    int flushResamplerToFifo();
    // 从音频 FIFO 取样拼装编码帧并写出；必要时补齐最后一帧。
    int drainAudioFifo(int flush_last_frame);
};

int transcode_file_with_separate_audio_to_mp4_cpp(
    const char *visual_input_path,
    const char *audio_input_path,
    const char *output_path,
    const FFmpegMp4TranscodeConfig *config,
    void (*progress_cb)(int percentage, void *user_data),
    void *user_data,
    volatile int *cancel_flag,
    volatile int *pause_flag
);

// 直接从媒体文件抽取音频流并重封装到目标容器；不重新编码，必要时只转换 AAC 头格式。
int extract_audio_stream_from_media_cpp(
    const char *input_path,
    const char *output_path,
    void (*progress_cb)(int percentage, void *user_data),
    void *user_data,
    volatile int *cancel_flag,
    volatile int *pause_flag
);

#endif /* AWESOME_FFMPEG_MP4_TRANSCODER_H */
