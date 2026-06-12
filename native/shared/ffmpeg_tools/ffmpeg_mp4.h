//
//  ffmpeg_mp4.h
//  AwesomeVideoKitSDK
//
//  Created by dev on 2026/3/20.
//

#ifndef AWESOME_FFMPEG_MP4_H
#define AWESOME_FFMPEG_MP4_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // x264 CRF，<= 0 时默认 23。
    int crf;
    // x264 preset，空时默认 "fast"。
    const char *preset;
    // H.264 profile，空时默认 "high"。
    const char *profile;
    // H.264 level，空时默认 "4.2"。
    const char *level;
    // AAC 码率，单位 bit/s，<= 0 时默认 128000。
    int audio_bitrate;
    // 是否开启 moov 前置；< 0 关闭，>= 0 或空配置默认开启。
    int faststart;
    // 输出帧率，单位 fps；<= 0 时保持源视频帧率不变。
    int frame_rate;
    // 输出视频宽度，<= 0 时沿用源视频宽度；仅设置一个维度时按原始宽高比自动补齐。
    int scale_width;
    // 输出视频高度，<= 0 时沿用源视频高度；仅设置一个维度时按原始宽高比自动补齐。
    int scale_height;
} FFmpegMp4TranscodeConfig;

/**
 * 将本地音视频文件转码为 mp4。
 *
 * 默认配置等价于：
 * ffmpeg -i input.mp4 -c:v libx264 -profile:v high -level 4.2 -pix_fmt yuv420p \
 *        -movflags +faststart -crf 23 -preset fast -c:a aac -b:a 128k output.mp4
 * iOS 真机优先尝试 VideoToolbox 硬编；如果硬编打开失败，会自动回退到软编码继续转码。
 *
 * 当 config->scale_width = 720 且 config->scale_height = 1280 时，等价于额外增加：
 * ffmpeg -vf "scale=720:1280"
 *
 * 当 config->frame_rate = 30 时，视频会按 30fps 输出；<= 0 时保持源视频帧率不变。
 *
 * @param input_path 输入文件路径
 * @param output_path 输出 mp4 文件路径
 * @param config 转码参数，传 NULL 时使用默认值
 * @param progress_cb 进度回调函数，参数是百分比 0~100，user_data 原样传入
 * @param user_data 传递给回调的用户数据指针
 * @param cancel_flag 用于外部控制取消转码
 * @param pause_flag 用于外部控制暂停/恢复转码，非 0 表示暂停
 * @return 0 成功，负数错误码
 */
int transcode_file_to_mp4(
    const char *input_path,
    const char *output_path,
    const FFmpegMp4TranscodeConfig *config,
    void (*progress_cb)(int percentage, void *user_data),
    void *user_data,
    volatile int *cancel_flag,
    volatile int *pause_flag
);

/**
 * 将“画面输入文件 + 独立音频文件”封装为 mp4。
 *
 * 画面输入既可以是普通视频，也可以是单帧图片：
 * 1. 当 visual_input_path 是视频时，仅保留其第一路视频流，并直接 copy 到输出 mp4；
 * 2. 当 visual_input_path 是图片时，会将图片编码为 H.264 静态视频轨，时长跟随输出时长；
 * 3. audio_input_path 只取第一路音频流；
 * 4. 当音频流已经是 AAC，且当前输出为 MP4 且未启用音频滤镜时，直接 copy 音频；
 * 5. 其他情况会把音频转码为 AAC；
 * 6. 输出时长遵循 `-shortest` 语义。
 *
 * @param visual_input_path 画面输入路径，支持视频或单帧图片
 * @param audio_input_path 独立音频输入路径
 * @param output_path 输出 mp4 文件路径
 * @param config 转码参数，传 NULL 时使用默认值
 * @param progress_cb 进度回调函数，参数是百分比 0~100，user_data 原样传入
 * @param user_data 传递给回调的用户数据指针
 * @param cancel_flag 用于外部控制取消转码
 * @param pause_flag 用于外部控制暂停/恢复转码，非 0 表示暂停
 * @return 0 成功，负数错误码
 */
int transcode_file_with_separate_audio_to_mp4(
    const char *visual_input_path,
    const char *audio_input_path,
    const char *output_path,
    const FFmpegMp4TranscodeConfig *config,
    void (*progress_cb)(int percentage, void *user_data),
    void *user_data,
    volatile int *cancel_flag,
    volatile int *pause_flag
);

/**
 * 从本地媒体文件中抽取首路音频流到目标容器。
 *
 * 默认仅做解复用/重封装，不做重新编码：
 * 1. input_path 只读取第一路音频流；
 * 2. output_path 的容器类型根据扩展名自动推断；
 * 3. 调用方需要保证目标容器支持该音频编码；
 * 4. 当输入 AAC 为 ADTS 且输出为 MP4/M4A 等 ISO BMFF 容器时，会自动应用 aac_adtstoasc。
 *
 * @param input_path 输入媒体文件路径
 * @param output_path 输出音频文件路径
 * @param progress_cb 进度回调函数，参数是百分比 0~100，user_data 原样传入
 * @param user_data 传递给回调的用户数据指针
 * @param cancel_flag 用于外部控制取消抽取
 * @param pause_flag 用于外部控制暂停/恢复抽取，非 0 表示暂停
 * @return 0 成功，负数错误码
 */
int extract_audio_stream_from_media(
    const char *input_path,
    const char *output_path,
    void (*progress_cb)(int percentage, void *user_data),
    void *user_data,
    volatile int *cancel_flag,
    volatile int *pause_flag
);

#ifdef __cplusplus
}
#endif

#endif /* AWESOME_FFMPEG_MP4_H */
