//
//  ffmpeg_hls_network.h
//  AwesomeVideoKitSDK
//
//  Created by dev on 2026/3/20.
//

#ifndef AWESOME_FFMPEG_HLS_NETWORK_H
#define AWESOME_FFMPEG_HLS_NETWORK_H

#include "ffmpeg_hls_common.h"

struct FFmpegResolvedInputUrl {
    std::string full_url;
    int64_t playlist_duration_us = 0;
    int playlist_is_vod = 0;
};

class FFmpegHlsNetworkUtils {
public:
    // 设置读取远程资源时通用的超时、重连和 UA 参数。
    static void setCommonNetworkOptions(AVDictionary **opts);
    // 通过路径后缀和内容头判断目标是否为 m3u8。
    static int urlIsM3u8(const char *url);
    // 读取远程文本资源并返回以 '\0' 结尾的字符串。
    static char *readUrlToString(const char *url);
    // 按分辨率关键字从 master m3u8 中提取子清单地址。
    static char *extractSubM3u8Url(const char *m3u8_content, const char *resolution_keyword);
    // 从 master m3u8 中挑选最接近目标高度的变体地址。
    static char *extractVariantM3u8UriFromMaster(const char *m3u8_content, int target_height);
    // 把 m3u8 中的相对路径解析成完整 URL。
    static char *resolveRelativeUrl(const char *base_url, const char *relative_path);
    // 当子地址缺少 query 时，继承原始 URL 的 query 参数。
    static char *inheritQueryIfMissing(const char *base_url, const char *url);
    // 解析 m3u8 总时长，并识别是否为 VOD 播放列表。
    static int64_t parseM3u8TotalDurationUs(const char *m3u8_content, int *is_vod_out);
    // 解析出真正可下载的输入地址以及播放列表元信息。
    static FFmpegResolvedInputUrl resolveEffectiveInputUrl(const char *m3u8_url);

private:
    // 忽略大小写检查 URL 路径是否匹配指定扩展名。
    static int urlPathHasExtensionCi(const char *url, const char *ext_no_dot);
    // 拷贝指定长度的字符串并补齐结尾 '\0'。
    static char *strndup0(const char *source, size_t length);
    // 去掉字符串片段首尾的空白和回车。
    static void trimSpan(const char **start, size_t *length);
    // 在给定长度的字符区间中查找子串。
    static int spanContains(const char *haystack, size_t haystack_length, const char *needle);
    // 返回给定长度字符区间里首次匹配子串的位置。
    static const char *spanFindStr0(const char *source, size_t source_length, const char *needle);
    // 判断给定长度字符区间里是否包含指定子串。
    static int spanContainsStr0(const char *source, size_t source_length, const char *needle);
    // 解析 key 后面的正整数值。
    static int parsePositiveIntAfterKey(const char *source, size_t source_length, const char *key, int *out);
    // 原地规范化 URL 中的路径段，消除 "." 和 ".."。
    static void normalizePathInplace(char *path);
};

#endif /* AWESOME_FFMPEG_HLS_NETWORK_H */
