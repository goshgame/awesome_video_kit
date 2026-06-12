//
//  ffmpeg_hls.cpp
//  AwesomeVideoKitSDK
//
//  Created by dev on 2026/3/20.
//

#include "ffmpeg_hls.h"

#include "ffmpeg_hls_downloader.h"

extern "C" {

int download_m3u8_to_mp4(
    const char *m3u8_url,
    const char *output_path,
    void (*progress_cb)(int percentage, void *user_data),
    void *user_data,
    volatile int *cancel_flag
) {
    FFmpegHlsDownloader downloader(
        m3u8_url,
        output_path,
        nullptr,
        progress_cb,
        user_data,
        cancel_flag
    );
    return downloader.download();
}

int download_m3u8_to_mp4_with_watermark(
    const char *m3u8_url,
    const char *output_path,
    const FFmpegWatermarkConfig *watermark,
    void (*progress_cb)(int percentage, void *user_data),
    void *user_data,
    volatile int *cancel_flag
) {
    FFmpegHlsDownloader downloader(
        m3u8_url,
        output_path,
        watermark,
        progress_cb,
        user_data,
        cancel_flag
    );
    return downloader.download();
}

} // extern "C"
