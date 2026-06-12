#ifndef AWESOME_FFMPEG_MP4_WATERMARK_TRANSCODER_H
#define AWESOME_FFMPEG_MP4_WATERMARK_TRANSCODER_H

#include "ffmpeg_hls.h"
#include "ffmpeg_mp4_concat_image.h"
#include "ffmpeg_mp4_transcoder_base.h"

// 本地文件图片水印专用转码器。
// 与 HLS 水印流水线分离，负责本地文件的 decode -> watermark filter -> encode，
// 并可在水印转码完成后继续在头部或尾部拼接一段静态图片。
class FFmpegMp4WatermarkTranscoder : public FFmpegMp4TranscoderBase {
public:
    // 保存一次本地文件加图片水印转 mp4 所需的输入参数和回调。
    FFmpegMp4WatermarkTranscoder(
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
    // 释放解复用、滤镜、编解码和音频转换阶段占用的资源。
    ~FFmpegMp4WatermarkTranscoder();

    // 执行完整转码流程，输出带图片水印的 H.264/AAC mp4 文件。
    int transcode();

private:
    std::string watermark_image_path_;
    int watermark_margin_x_;
    int watermark_margin_y_;
    int watermark_width_;
    int watermark_height_;
    FFmpegWatermarkPosition watermark_position_;
    std::string concat_image_path_;
    int64_t concat_image_duration_us_;
    FFmpegConcatImagePosition concat_image_position_;
    AVFrame *video_enc_frame_;
    AVFrame *concat_image_frame_;
    AVFilterGraph *filter_graph_;
    AVFilterContext *buffer_src_ctx_;
    AVFilterContext *buffer_sink_ctx_;
    AVFrame *filtered_video_frame_;
    AVFrame *last_video_frame_;
    int64_t next_video_pts_;
    int64_t video_segment_offset_pts_;
    AVRational filter_time_base_;

    // 释放当前转码任务已申请的全部 FFmpeg 对象。
    void cleanup();
    // 判断当前任务是否配置了有效的图片水印路径。
    bool hasWatermark() const;
    // 判断当前任务是否需要在转码完成后继续拼接静态图片段。
    bool hasConcatImage() const;

    // 打开输入文件、读取流信息并初始化进度状态。
    int openInput();
    // 创建输出容器并初始化编码器、滤镜图、帧缓冲和输出 IO。
    int openOutput();
    // 初始化视频编码器、输出视频流以及水印滤镜图。
    int initializeVideoEncoder();
    // 复用当前配置重新打开视频编码器；首次创建时同步建立输出视频流和滤镜图。
    int configureVideoEncoder(bool create_output_stream);
    // 初始化 AAC 编码器、重采样器和音频 FIFO。
    int initializeAudioEncoder();
    // 当没有 AAC 编码器但输入已经是 AAC 时，直接复制音频流。
    int initializeAudioCopyStream();
    // 为视频滤镜链路和音频解码阶段申请复用帧对象。
    int initializeFrames();
    // 主循环读取输入 packet，驱动音视频解码、滤镜、转换和编码。
    int transcodeLoop();

    // 处理一个输入视频包，完成解码并推进滤镜和编码链路。
    int processVideoPacket(AVPacket *packet);
    // 处理一个输入音频包，完成解码并推进后续编码链路。
    int processAudioPacket(AVPacket *packet);
    // 直接复制输入 AAC 音频包到输出 mp4。
    int processAudioPacketCopy(AVPacket *packet);
    // 归一化视频时间戳、更新进度，并把视频帧送入水印滤镜图。
    int processDecodedVideoFrame(AVFrame *frame);
    // 把解码后音频重采样后写入 FIFO，并尝试输出编码帧。
    int processDecodedAudioFrame(AVFrame *frame);

    // 刷新视频解码器、滤镜图和编码器中的剩余帧。
    int flushVideoPipeline();
    // 刷新音频解码器、重采样器、FIFO 和编码器中的剩余数据。
    int flushAudioPipeline();
    // 关闭视频编码器并写出尾包。
    int finalizeVideoEncoder();
    // 关闭音频编码器并写出尾包。
    int finalizeAudioEncoder();
    // 把重采样器内部缓存的尾部采样刷入音频 FIFO。
    int flushResamplerToFifo();
    // 从音频 FIFO 取样拼装编码帧并写出；必要时补齐最后一帧。
    int drainAudioFifo(int flush_last_frame);
    // 把目标帧送入编码器，并缓存最近一帧供补帧使用。
    int sendPreparedVideoFrame(AVFrame *frame, int64_t output_pts);
    // 当目标帧率高于源帧率时，复制上一帧填补中间的时间空洞。
    int duplicateLastVideoFrameUntil(int64_t target_pts);
    // 把滤镜输出帧换算到编码器时间基后送入视频编码器。
    int sendFilteredFrameToEncoder(AVFrame *frame);
    // 持续拉取水印滤镜输出帧并送入编码器。
    int drainFilterSink();
    // 把解码视频帧送入滤镜图并立即消费滤镜输出。
    int pushFrameToFilterAndEncode(AVFrame *frame);
    // 执行仅包含“加水印”的第一阶段转码。
    int transcodeWatermarkStage();
    // 初始化拼接图片对应的编码帧。
    int initializeConcatImageFrame();
    // 生成头部或尾部静态图片视频段。
    int generateConcatImageVideoSegment(int64_t start_time_us);
    // 为静态图片段生成对应的静音音频。
    int appendSilentAudioSegment(int64_t duration_us);
    // 在主视频前面插入静态图片段。
    int prependConcatImageSegmentIfNeeded();
    // 在主视频后面插入静态图片段。
    int appendConcatImageSegmentIfNeeded();
    // 估算当前视频输出时间。
    int64_t currentVideoOutputTimeUs() const;
    // 估算当前音频输出时间。
    int64_t currentAudioOutputTimeUs() const;
    // VideoToolbox 等硬件编码器失效后，重建视频编码器和滤镜图并续写时间轴。
    int rebuildVideoEncoderForResume() override;
};

#endif /* AWESOME_FFMPEG_MP4_WATERMARK_TRANSCODER_H */
