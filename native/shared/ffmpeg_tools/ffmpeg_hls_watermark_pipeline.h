//
//  ffmpeg_hls_watermark_pipeline.h
//  AwesomeVideoKitSDK
//
//  Created by dev on 2026/3/20.
//

#ifndef AWESOME_FFMPEG_HLS_WATERMARK_PIPELINE_H
#define AWESOME_FFMPEG_HLS_WATERMARK_PIPELINE_H

#include "ffmpeg_hls_common.h"
#include "ffmpeg_hls_progress.h"

#include <vector>

class FFmpegWatermarkPipeline {
public:
    // 初始化水印流水线的成员状态。
    FFmpegWatermarkPipeline();
    // 释放解码、滤镜和编码阶段占用的资源。
    ~FFmpegWatermarkPipeline();

    // 判断当前配置是否真正启用了图片水印。
    static int isEnabled(const FFmpegWatermarkConfig *watermark);

    // 初始化解码器、编码器和 overlay 滤镜图。
    int initialize(
        AVFormatContext *ifmt,
        AVFormatContext *ofmt,
        int video_input_index,
        int video_output_index,
        const FFmpegWatermarkConfig *watermark
    );

    // 处理一个输入视频包并输出编码后的视频包。
    int processPacket(
        AVPacket *packet,
        AVFormatContext *output_format,
        FFmpegProgressTracker *progress
    );

    // 刷新解码器、滤镜和编码器中的剩余帧。
    int flush(AVFormatContext *output_format, FFmpegProgressTracker *progress);

private:
#if AWESOME_HAS_AVFILTER
    AVCodecContext *dec_ctx_;
    AVCodecContext *enc_ctx_;
    AVFilterGraph *filter_graph_;
    AVFilterContext *buffer_src_ctx_;
    AVFilterContext *buffer_sink_ctx_;
    AVFrame *dec_frame_;
    AVFrame *filtered_frame_;
    AVPacket *enc_packet_;
    int input_stream_index_;
    int output_stream_index_;
    AVRational filter_time_base_;
    AVFormatContext *input_format_;
    AVFormatContext *output_format_;
    const FFmpegWatermarkConfig *watermark_;
    AVRational frame_rate_;
    int64_t video_encoder_input_origin_pts_;
    int64_t video_encoder_output_offset_pts_;
    int64_t video_last_written_pts_;
    int64_t video_last_written_dts_;
    int video_last_write_packet_count_;
    bool video_force_keyframe_on_next_frame_;
    bool video_packets_written_;
    bool video_muxer_reopen_required_;
    std::vector<uint8_t> video_extradata_snapshot_;

    // 释放当前流水线已申请的全部 FFmpeg 对象。
    void cleanup();
    // 构建主视频和水印图片参与的滤镜图。
    int initializeFilterGraph(AVStream *video_input_stream, const FFmpegWatermarkConfig *watermark);
    // 根据当前配置打开或重建视频编码器，并刷新输出视频流参数。
    int configureEncoder();
    // 拉取编码器输出包并写入目标容器。
    int writeEncodedVideoPackets(AVFormatContext *output_format);
    // 把滤镜输出帧送入编码器并写出结果。
    int sendFilteredFrameToEncoder(AVFrame *frame, AVFormatContext *output_format);
    // 持续读取滤镜输出帧并更新进度。
    int drainFilterSink(AVFormatContext *output_format, FFmpegProgressTracker *progress);
    // 把视频帧送入滤镜图并串联后续编码输出。
    int pushFrameToFilterAndEncode(AVFrame *frame, AVFormatContext *output_format, FFmpegProgressTracker *progress);
    // 硬件编码器失效后，重建编码器和滤镜图并续写时间轴。
    void markEncoderRebuilt();
#endif
};

#endif /* AWESOME_FFMPEG_HLS_WATERMARK_PIPELINE_H */
