#ifndef AWESOME_FFMPEG_AV_FILE_INFO_H
#define AWESOME_FFMPEG_AV_FILE_INFO_H

#include <stdint.h>

#ifdef __cplusplus

#include <string>
#include <vector>

// 资源基础信息探测类。
// 负责使用 FFmpeg 读取本地或网络资源的容器/流信息，并以稳定的 C++ 结构对外提供查询能力。
class FFmpegAVFileInfo {
public:
    enum FileType {
        FileTypeUnknown = 0,
        FileTypeAudio = 1,
        FileTypeVideo = 2,
        FileTypeImage = 3,
    };

    struct Size {
        int width = 0;
        int height = 0;
    };

    struct Rational {
        int num = 0;
        int den = 1;
    };

    enum VideoRotation {
        VideoRotationUnknown = -1,
        VideoRotation0 = 0,
        VideoRotation90 = 90,
        VideoRotation180 = 180,
        VideoRotation270 = 270,
    };

    enum VideoCodecType {
        VideoCodecTypeUnknown = 0,
        VideoCodecTypeH264,
        VideoCodecTypeHEVC,
        VideoCodecTypeMPEG4,
        VideoCodecTypeMPEG2,
        VideoCodecTypeH263,
        VideoCodecTypeVP8,
        VideoCodecTypeVP9,
        VideoCodecTypeAV1,
        VideoCodecTypeMJPEG,
        VideoCodecTypePNG,
        VideoCodecTypeProRes,
        VideoCodecTypeDNxHD,
        VideoCodecTypeVC1,
        VideoCodecTypeWMV3,
        VideoCodecTypeOther,
    };

    enum VideoColorTransfer {
        VideoColorTransferUnknown = 0,
        VideoColorTransferBT709,
        VideoColorTransferGamma22,
        VideoColorTransferGamma28,
        VideoColorTransferSMPTE170M,
        VideoColorTransferSMPTE240M,
        VideoColorTransferLinear,
        VideoColorTransferLog,
        VideoColorTransferLogSqrt,
        VideoColorTransferIEC61966_2_4,
        VideoColorTransferBT1361ECG,
        VideoColorTransferSRGB,
        VideoColorTransferBT2020_10,
        VideoColorTransferBT2020_12,
        VideoColorTransferPQ,
        VideoColorTransferHLG,
        VideoColorTransferOther,
    };

    enum VideoHDRType {
        VideoHDRTypeUnknown = 0,
        VideoHDRTypeSDR,
        VideoHDRTypeHDR10,
        VideoHDRTypeHDR10Plus,
        VideoHDRTypeHLG,
        VideoHDRTypeDolbyVision,
    };

    struct VideoStreamInfo {
        int64_t duration_us = 0;
        Size dimension;
        Rational pixel_aspect_ratio;
        Rational frame_rate;
        VideoRotation rotation = VideoRotationUnknown;
        int rotation_degrees = 0;
        unsigned int component_bit_count = 0;
        VideoCodecType codec_type = VideoCodecTypeUnknown;
        std::string codec_name;
        int codec_profile = -1;
        int codec_level = -1;
        VideoColorTransfer color_transfer = VideoColorTransferUnknown;
        VideoHDRType hdr_type = VideoHDRTypeUnknown;
    };

    struct AudioStreamInfo {
        int64_t duration_us = 0;
        unsigned int sample_rate = 0;
        unsigned int channel_count = 0;
        bool codec_supported = false;
        std::string codec_name;
    };

    // 构造一个空的资源信息对象。
    FFmpegAVFileInfo();

    // 清空当前已加载的资源信息，恢复到初始状态。
    void clear();
    /**
     * 从本地路径或网络 URL 加载容器和流信息。
     * @param file_path 本地资源路径或网络 URL
     * @return 0 成功，负数为 FFmpeg 错误码；失败时对象会被清空
     */
    int loadFromFile(const char *file_path);

    // 返回当前资源类型；未加载或无法识别时返回 FileTypeUnknown。
    FileType avFileType() const;
    // 返回资源总时长，单位微秒。
    int64_t duration() const;
    // 返回资源整体码率，单位 bit/s。
    uint64_t dataRate() const;
    // 返回视频流数量。
    unsigned int videoStreamCount() const;
    // 返回音频流数量。
    unsigned int audioStreamCount() const;
    // 返回最近一次成功加载的源文件路径；未加载时为空字符串。
    const std::string &sourcePath() const;

    // 返回指定视频流时长，单位微秒；索引无效时返回 0。
    int64_t getVideoStreamDuration(unsigned int video_stream_index) const;
    // 返回指定视频流分辨率；索引无效时返回 {0, 0}。
    Size getVideoStreamDimension(unsigned int video_stream_index) const;
    // 返回指定视频流像素宽高比；索引无效时返回 {0, 1}。
    Rational getVideoStreamPixelAspectRatio(unsigned int video_stream_index) const;
    // 返回指定视频流帧率；索引无效时返回 {0, 1}。
    Rational getVideoStreamFrameRate(unsigned int video_stream_index) const;
    // 返回指定视频流旋转枚举值；索引无效时返回 VideoRotationUnknown。
    VideoRotation getVideoStreamRotation(unsigned int video_stream_index) const;
    // 返回指定视频流归一化后的旋转角度；索引无效时返回 0。
    int getVideoStreamRotationDegrees(unsigned int video_stream_index) const;
    // 返回指定视频流单个颜色分量位深；索引无效时返回 0。
    unsigned int getVideoStreamComponentBitCount(unsigned int video_stream_index) const;
    // 返回指定视频流编码类型；索引无效时返回 VideoCodecTypeUnknown。
    VideoCodecType getVideoStreamCodecType(unsigned int video_stream_index) const;
    // 返回指定视频流编码名称；索引无效时返回空字符串。
    const char *getVideoStreamCodecName(unsigned int video_stream_index) const;
    // 返回指定视频流编码 profile；未知或索引无效时返回 -1。
    int getVideoCodecProfile(unsigned int video_stream_index) const;
    // 返回指定视频流编码 level；未知或索引无效时返回 -1。
    int getVideoCodecLevel(unsigned int video_stream_index) const;
    // 返回指定视频流传输特性；索引无效时返回 VideoColorTransferUnknown。
    VideoColorTransfer getVideoStreamColorTransfer(unsigned int video_stream_index) const;
    // 返回指定视频流 HDR 类型；索引无效时返回 VideoHDRTypeUnknown。
    VideoHDRType getVideoStreamHDRType(unsigned int video_stream_index) const;

    // 返回指定音频流时长，单位微秒；索引无效时返回 0。
    int64_t getAudioStreamDuration(unsigned int audio_stream_index) const;
    // 返回指定音频流采样率；索引无效时返回 0。
    unsigned int getAudioStreamSampleRate(unsigned int audio_stream_index) const;
    // 返回指定音频流声道数；索引无效时返回 0。
    unsigned int getAudioStreamChannelCount(unsigned int audio_stream_index) const;
    // 返回当前 FFmpeg 构建是否支持指定音频流对应解码器；索引无效时返回 false。
    bool getAudioStreamCodecSupport(unsigned int audio_stream_index) const;
    // 返回指定音频流编码名称；索引无效时返回空字符串。
    const char *getAudioStreamCodecName(unsigned int audio_stream_index) const;

    // 返回指定视频流完整信息指针；索引无效时返回 nullptr。
    const VideoStreamInfo *getVideoStreamInfo(unsigned int video_stream_index) const;
    // 返回指定音频流完整信息指针；索引无效时返回 nullptr。
    const AudioStreamInfo *getAudioStreamInfo(unsigned int audio_stream_index) const;

private:
    std::string source_path_;
    FileType av_file_type_;
    int64_t duration_;
    uint64_t data_rate_;
    std::vector<VideoStreamInfo> video_streams_;
    std::vector<AudioStreamInfo> audio_streams_;
};

#endif

#endif /* AWESOME_FFMPEG_AV_FILE_INFO_H */
