//
//  ffmpeg_hls_common.h
//  AwesomeVideoKitSDK
//
//  Created by dev on 2026/3/20.
//

#ifndef AWESOME_FFMPEG_HLS_COMMON_H
#define AWESOME_FFMPEG_HLS_COMMON_H

#include "ffmpeg_hls.h"

// FFmpeg 内部公共聚合头文件。
// 统一收敛 HLS 下载、转码、水印等模块依赖的 FFmpeg 头，避免各实现文件重复维护。
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/avutil.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

// 图片水印依赖 libavfilter，但并非所有平台包都会带上 avfilter 相关头文件和库。
// 这里做编译期开关控制：如果当前 FFmpeg 裁剪版本不包含 avfilter，
// 则水印功能编译为不可用，同时保证下载和普通转码能力仍可正常构建。
#if __has_include(<libavfilter/avfilter.h>) && __has_include(<libavfilter/buffersink.h>) && __has_include(<libavfilter/buffersrc.h>)
#define AWESOME_HAS_AVFILTER 1
extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
}
#else
#define AWESOME_HAS_AVFILTER 0
#endif

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

#endif /* AWESOME_FFMPEG_HLS_COMMON_H */
