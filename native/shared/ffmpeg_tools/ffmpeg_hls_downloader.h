//
//  ffmpeg_hls_downloader.h
//  AwesomeVideoKitSDK
//
//  Created by dev on 2026/3/20.
//

#ifndef AWESOME_FFMPEG_HLS_DOWNLOADER_H
#define AWESOME_FFMPEG_HLS_DOWNLOADER_H

#include "ffmpeg_hls_common.h"

class FFmpegHlsDownloader {
public:
    // 保存一次下载任务所需的输入参数和回调。
    FFmpegHlsDownloader(
        const char *m3u8_url,
        const char *output_path,
        const FFmpegWatermarkConfig *watermark,
        void (*progress_cb)(int percentage, void *user_data),
        void *user_data,
        volatile int *cancel_flag
    );

    // 执行 m3u8 下载，必要时串联水印处理和进度回调。
    int download();

private:
    const char *m3u8_url_;
    const char *output_path_;
    const FFmpegWatermarkConfig *watermark_;
    void (*progress_cb_)(int percentage, void *user_data);
    void *user_data_;
    volatile int *cancel_flag_;
};

#endif /* AWESOME_FFMPEG_HLS_DOWNLOADER_H */
