//
//  ffmpeg_hls_network.cpp
//  AwesomeVideoKitSDK
//
//  Created by dev on 2026/3/20.
//

#include "ffmpeg_hls_network.h"

#include "ffmpeg_common_utils.h"

void FFmpegHlsNetworkUtils::setCommonNetworkOptions(AVDictionary **opts) {
    if (!opts) return;
    av_dict_set(opts, "user_agent", "Mozilla/5.0 (Linux; Android 6.0; Nexus 5 Build/MRA58N) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/143.0.0.0 Mobile Safari/537.36", 0);
    av_dict_set(opts, "timeout", "5000000", 0);
    av_dict_set(opts, "rw_timeout", "5000000", 0);
    av_dict_set(opts, "reconnect", "1", 0);
    av_dict_set(opts, "follow_redirects", "1", 0);
}

int FFmpegHlsNetworkUtils::urlPathHasExtensionCi(const char *url, const char *ext_no_dot) {
    if (!url || !*url || !ext_no_dot || !*ext_no_dot) return 0;

    const char *end = url;
    while (*end && *end != '?' && *end != '#') end++;

    const char *last_slash = url;
    for (const char *cursor = url; cursor < end; ++cursor) {
        if (*cursor == '/') last_slash = cursor + 1;
    }

    const char *dot = nullptr;
    for (const char *cursor = last_slash; cursor < end; ++cursor) {
        if (*cursor == '.') dot = cursor;
    }
    if (!dot || dot + 1 >= end) return 0;

    ++dot;
    const size_t ext_length = static_cast<size_t>(end - dot);
    const size_t expected_length = strlen(ext_no_dot);
    if (ext_length != expected_length) return 0;

    for (size_t index = 0; index < expected_length; ++index) {
        if (tolower(static_cast<unsigned char>(dot[index])) !=
            tolower(static_cast<unsigned char>(ext_no_dot[index]))) {
            return 0;
        }
    }
    return 1;
}

int FFmpegHlsNetworkUtils::urlIsM3u8(const char *url) {
    if (!url || !*url) return 0;
    if (urlPathHasExtensionCi(url, "m3u8")) return 1;

    AVIOContext *io_ctx = nullptr;
    AVDictionary *opts = nullptr;
    setCommonNetworkOptions(&opts);

    const int ret = avio_open2(&io_ctx, url, AVIO_FLAG_READ, nullptr, &opts);
    av_dict_free(&opts);
    if (ret < 0) return 0;

    unsigned char buffer[128];
    const int bytes_read = avio_read(io_ctx, buffer, sizeof(buffer));
    avio_closep(&io_ctx);
    if (bytes_read <= 0) return 0;

    int offset = 0;
    if (bytes_read >= 3 && buffer[0] == 0xEF && buffer[1] == 0xBB && buffer[2] == 0xBF) {
        offset = 3;
    }
    while (offset < bytes_read &&
           (buffer[offset] == ' ' || buffer[offset] == '\t' || buffer[offset] == '\r' || buffer[offset] == '\n')) {
        ++offset;
    }

    static const char kMagic[] = "#EXTM3U";
    return offset + static_cast<int>(sizeof(kMagic)) - 1 <= bytes_read &&
        memcmp(buffer + offset, kMagic, sizeof(kMagic) - 1) == 0;
}

char *FFmpegHlsNetworkUtils::strndup0(const char *source, size_t length) {
    char *out = static_cast<char *>(malloc(length + 1));
    if (!out) return nullptr;
    memcpy(out, source, length);
    out[length] = '\0';
    return out;
}

void FFmpegHlsNetworkUtils::trimSpan(const char **start, size_t *length) {
    if (!start || !*start || !length) return;
    while (*length > 0 && isspace(static_cast<unsigned char>((*start)[0]))) {
        ++(*start);
        --(*length);
    }
    while (*length > 0) {
        const char value = (*start)[*length - 1];
        if (value == '\r' || isspace(static_cast<unsigned char>(value))) {
            --(*length);
            continue;
        }
        break;
    }
}

int FFmpegHlsNetworkUtils::spanContains(const char *haystack, size_t haystack_length, const char *needle) {
    if (!haystack || !needle) return 0;
    const size_t needle_length = strlen(needle);
    if (needle_length == 0) return 1;
    if (needle_length > haystack_length) return 0;

    for (size_t index = 0; index + needle_length <= haystack_length; ++index) {
        if (memcmp(haystack + index, needle, needle_length) == 0) return 1;
    }
    return 0;
}

char *FFmpegHlsNetworkUtils::readUrlToString(const char *url) {
    if (!url || !*url) return nullptr;

    AVIOContext *io_ctx = nullptr;
    AVDictionary *opts = nullptr;
    setCommonNetworkOptions(&opts);

    const int ret = avio_open2(&io_ctx, url, AVIO_FLAG_READ, nullptr, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        char error_buffer[128];
        av_strerror(ret, error_buffer, sizeof(error_buffer));
        AWESOME_FF_LOGE("Failed to open URL %s: %s", url, error_buffer);
        return nullptr;
    }

    const size_t chunk_size = 4096;
    size_t capacity = 16 * 1024;
    size_t length = 0;
    char *buffer = static_cast<char *>(malloc(capacity + 1));
    if (!buffer) {
        avio_closep(&io_ctx);
        return nullptr;
    }

    for (;;) {
        if (length + chunk_size + 1 > capacity) {
            size_t next_capacity = capacity;
            while (length + chunk_size + 1 > next_capacity) {
                if (next_capacity > (16 * 1024 * 1024)) {
                    AWESOME_FF_LOGE("m3u8 content too large, aborting");
                    free(buffer);
                    avio_closep(&io_ctx);
                    return nullptr;
                }
                next_capacity *= 2;
            }

            char *next_buffer = static_cast<char *>(realloc(buffer, next_capacity + 1));
            if (!next_buffer) {
                free(buffer);
                avio_closep(&io_ctx);
                return nullptr;
            }
            buffer = next_buffer;
            capacity = next_capacity;
        }

        const int bytes_read = avio_read(io_ctx, reinterpret_cast<unsigned char *>(buffer + length), static_cast<int>(chunk_size));
        if (bytes_read == 0 || bytes_read == AVERROR_EOF) break;
        if (bytes_read < 0) {
            char error_buffer[128];
            av_strerror(bytes_read, error_buffer, sizeof(error_buffer));
            AWESOME_FF_LOGE("Error reading URL %s: %s", url, error_buffer);
            free(buffer);
            avio_closep(&io_ctx);
            return nullptr;
        }
        length += static_cast<size_t>(bytes_read);
    }

    avio_closep(&io_ctx);
    buffer[length] = '\0';
    return buffer;
}

char *FFmpegHlsNetworkUtils::extractSubM3u8Url(const char *m3u8_content, const char *resolution_keyword) {
    if (!m3u8_content || !*m3u8_content || !resolution_keyword || !*resolution_keyword) return nullptr;

    const char *cursor = m3u8_content;
    while (*cursor) {
        const char *line_start = cursor;
        const char *line_end = strchr(cursor, '\n');
        size_t line_length = line_end ? static_cast<size_t>(line_end - line_start) : strlen(line_start);
        cursor = line_end ? (line_end + 1) : (line_start + line_length);

        trimSpan(&line_start, &line_length);
        if (line_length == 0) continue;
        if (!spanContains(line_start, line_length, resolution_keyword)) continue;

        while (*cursor) {
            const char *url_start = cursor;
            const char *url_end = strchr(cursor, '\n');
            size_t url_length = url_end ? static_cast<size_t>(url_end - url_start) : strlen(url_start);
            cursor = url_end ? (url_end + 1) : (url_start + url_length);

            trimSpan(&url_start, &url_length);
            if (url_length == 0) continue;
            if (url_start[0] == '#') continue;
            return strndup0(url_start, url_length);
        }
        return nullptr;
    }

    return nullptr;
}

const char *FFmpegHlsNetworkUtils::spanFindStr0(const char *source, size_t source_length, const char *needle) {
    if (!source || !needle) return nullptr;
    const size_t needle_length = strlen(needle);
    if (needle_length == 0) return source;
    if (needle_length > source_length) return nullptr;

    for (size_t index = 0; index + needle_length <= source_length; ++index) {
        if (memcmp(source + index, needle, needle_length) == 0) return source + index;
    }
    return nullptr;
}

int FFmpegHlsNetworkUtils::spanContainsStr0(const char *source, size_t source_length, const char *needle) {
    return spanFindStr0(source, source_length, needle) != nullptr;
}

int FFmpegHlsNetworkUtils::parsePositiveIntAfterKey(
    const char *source,
    size_t source_length,
    const char *key,
    int *out
) {
    if (!out) return 0;
    *out = 0;

    const char *cursor = spanFindStr0(source, source_length, key);
    if (!cursor) return 0;
    cursor += strlen(key);

    const char *end = source + source_length;
    if (cursor >= end) return 0;

    long value = 0;
    int has_any = 0;
    while (cursor < end && isdigit(static_cast<unsigned char>(*cursor))) {
        has_any = 1;
        value = (value * 10) + (*cursor - '0');
        if (value > INT_MAX) return 0;
        ++cursor;
    }

    if (!has_any || value <= 0) return 0;
    *out = static_cast<int>(value);
    return 1;
}

char *FFmpegHlsNetworkUtils::extractVariantM3u8UriFromMaster(const char *m3u8_content, int target_height) {
    if (!m3u8_content || !*m3u8_content) return nullptr;

    struct Candidate {
        int height = 0;
        int bandwidth = 0;
        char *uri = nullptr;
    };

    Candidate best_under;
    Candidate best_over;

    const char *cursor = m3u8_content;
    while (*cursor) {
        const char *line_start = cursor;
        const char *line_end = strchr(cursor, '\n');
        size_t line_length = line_end ? static_cast<size_t>(line_end - line_start) : strlen(line_start);
        cursor = line_end ? (line_end + 1) : (line_start + line_length);

        while (line_length > 0 && (line_start[0] == '\r' || isspace(static_cast<unsigned char>(line_start[0])))) {
            ++line_start;
            --line_length;
        }
        while (line_length > 0 &&
               (line_start[line_length - 1] == '\r' || isspace(static_cast<unsigned char>(line_start[line_length - 1])))) {
            --line_length;
        }
        if (line_length == 0) continue;
        if (line_length < 18 || memcmp(line_start, "#EXT-X-STREAM-INF", 17) != 0) continue;

        int height = 0;
        int bandwidth = 0;

        const char *resolution = spanFindStr0(line_start, line_length, "RESOLUTION=");
        if (resolution) {
            resolution += strlen("RESOLUTION=");
            const char *end = line_start + line_length;
            const char *width_separator = static_cast<const char *>(memchr(resolution, 'x', static_cast<size_t>(end - resolution)));
            if (!width_separator) {
                width_separator = static_cast<const char *>(memchr(resolution, 'X', static_cast<size_t>(end - resolution)));
            }
            if (width_separator && width_separator + 1 < end) {
                long value = 0;
                int has_any = 0;
                const char *digit = width_separator + 1;
                while (digit < end && isdigit(static_cast<unsigned char>(*digit))) {
                    has_any = 1;
                    value = (value * 10) + (*digit - '0');
                    if (value > INT_MAX) {
                        has_any = 0;
                        break;
                    }
                    ++digit;
                }
                if (has_any && value > 0) {
                    height = static_cast<int>(value);
                }
            }
        }

        parsePositiveIntAfterKey(line_start, line_length, "BANDWIDTH=", &bandwidth);

        const char *uri_cursor = cursor;
        while (*uri_cursor) {
            const char *uri_start = uri_cursor;
            const char *uri_end = strchr(uri_cursor, '\n');
            size_t uri_length = uri_end ? static_cast<size_t>(uri_end - uri_start) : strlen(uri_start);
            uri_cursor = uri_end ? (uri_end + 1) : (uri_start + uri_length);

            while (uri_length > 0 && (uri_start[0] == '\r' || isspace(static_cast<unsigned char>(uri_start[0])))) {
                ++uri_start;
                --uri_length;
            }
            while (uri_length > 0 &&
                   (uri_start[uri_length - 1] == '\r' || isspace(static_cast<unsigned char>(uri_start[uri_length - 1])))) {
                --uri_length;
            }
            if (uri_length == 0) continue;
            if (uri_start[0] == '#') continue;

            char *uri = static_cast<char *>(malloc(uri_length + 1));
            if (!uri) break;
            memcpy(uri, uri_start, uri_length);
            uri[uri_length] = '\0';

            cursor = uri_cursor;

            if (height > 0) {
                if (height <= target_height) {
                    if (!best_under.uri || height > best_under.height) {
                        if (best_under.uri) free(best_under.uri);
                        best_under = Candidate{height, bandwidth, uri};
                    } else {
                        free(uri);
                    }
                } else {
                    if (!best_over.uri || height > best_over.height) {
                        if (best_over.uri) free(best_over.uri);
                        best_over = Candidate{height, bandwidth, uri};
                    } else {
                        free(uri);
                    }
                }
            } else {
                if (!best_under.uri || bandwidth > best_under.bandwidth) {
                    if (best_under.uri) free(best_under.uri);
                    best_under = Candidate{0, bandwidth, uri};
                } else {
                    free(uri);
                }
            }
            break;
        }
    }

    if (best_under.uri) {
        if (best_over.uri) free(best_over.uri);
        return best_under.uri;
    }
    return best_over.uri;
}

void FFmpegHlsNetworkUtils::normalizePathInplace(char *path) {
    if (!path || !*path) return;

    char *destination = path;
    char *source = path;

    if (*source == '/') *destination++ = *source++;

    while (*source) {
        while (*source == '/') ++source;
        if (!*source) break;

        char *segment = source;
        while (*source && *source != '/') ++source;
        const size_t segment_length = static_cast<size_t>(source - segment);

        if (segment_length == 1 && segment[0] == '.') continue;
        if (segment_length == 2 && segment[0] == '.' && segment[1] == '.') {
            if (destination > path + 1) {
                --destination;
                while (destination > path && destination[-1] != '/') --destination;
            }
            continue;
        }

        if (destination > path && destination[-1] != '/') *destination++ = '/';
        memmove(destination, segment, segment_length);
        destination += segment_length;
    }

    if (destination > path + 1 && destination[-1] == '/') --destination;
    *destination = '\0';
}

char *FFmpegHlsNetworkUtils::resolveRelativeUrl(const char *base_url, const char *relative_path) {
    if (!base_url || !*base_url || !relative_path || !*relative_path) return nullptr;

    const char *relative = relative_path;
    size_t relative_length = strlen(relative);
    trimSpan(&relative, &relative_length);
    if (relative_length == 0) return nullptr;

    if (spanContains(relative, relative_length, "://")) return strndup0(relative, relative_length);

    const char *scheme_separator = strstr(base_url, "://");
    if (!scheme_separator) return nullptr;
    const char *host_start = scheme_separator + 3;
    const char *host_end = strpbrk(host_start, "/?#");
    if (!host_end) host_end = base_url + strlen(base_url);
    const size_t origin_length = static_cast<size_t>(host_end - base_url);

    const char *base_end_without_query = strpbrk(base_url, "?#");
    if (!base_end_without_query) base_end_without_query = base_url + strlen(base_url);
    const size_t base_without_query_length = static_cast<size_t>(base_end_without_query - base_url);

    if (relative_length >= 2 && relative[0] == '/' && relative[1] == '/') {
        const size_t scheme_length = static_cast<size_t>(scheme_separator - base_url);
        const size_t output_length = scheme_length + 1 + relative_length;
        char *out = static_cast<char *>(malloc(output_length + 1));
        if (!out) return nullptr;
        memcpy(out, base_url, scheme_length);
        out[scheme_length] = ':';
        memcpy(out + scheme_length + 1, relative, relative_length);
        out[output_length] = '\0';
        return out;
    }

    if (relative[0] == '?' || relative[0] == '#') {
        char *out = static_cast<char *>(malloc(base_without_query_length + relative_length + 1));
        if (!out) return nullptr;
        memcpy(out, base_url, base_without_query_length);
        memcpy(out + base_without_query_length, relative, relative_length);
        out[base_without_query_length + relative_length] = '\0';
        return out;
    }

    const char *base_path = origin_length < base_without_query_length ? (base_url + origin_length) : "";
    size_t base_path_length = origin_length < base_without_query_length
        ? static_cast<size_t>(base_without_query_length - origin_length)
        : 0;
    if (base_path_length == 0) {
        base_path = "/";
        base_path_length = 1;
    }

    size_t base_directory_length = 1;
    if (base_path_length > 0 && base_path[0] == '/') {
        base_directory_length = base_path_length;
        while (base_directory_length > 1 && base_path[base_directory_length - 1] != '/') {
            --base_directory_length;
        }
    }

    const int relative_is_absolute = (relative[0] == '/');
    const size_t output_length = origin_length + (relative_is_absolute ? 0 : base_directory_length) + relative_length;
    char *out = static_cast<char *>(malloc(output_length + 1));
    if (!out) return nullptr;

    memcpy(out, base_url, origin_length);
    size_t position = origin_length;

    if (!relative_is_absolute) {
        memcpy(out + position, base_path, base_directory_length);
        position += base_directory_length;
    }

    memcpy(out + position, relative, relative_length);
    position += relative_length;
    out[position] = '\0';

    char *output_path = strchr(out + origin_length, '/');
    if (output_path) {
        char *query_or_fragment = strpbrk(output_path, "?#");
        char *tail = nullptr;
        if (query_or_fragment) {
            tail = strdup(query_or_fragment);
            *query_or_fragment = '\0';
        }
        normalizePathInplace(output_path);
        if (tail) {
            strcat(out, tail);
            free(tail);
        }
    }

    return out;
}

char *FFmpegHlsNetworkUtils::inheritQueryIfMissing(const char *base_url, const char *url) {
    if (!base_url || !url) return nullptr;
    if (strchr(url, '?') != nullptr) return strdup(url);

    const char *base_query = strchr(base_url, '?');
    if (!base_query) return strdup(url);

    const char *base_query_end = strchr(base_query, '#');
    const size_t base_query_length = base_query_end ? static_cast<size_t>(base_query_end - base_query) : strlen(base_query);

    const char *fragment = strchr(url, '#');
    const size_t url_prefix_length = fragment ? static_cast<size_t>(fragment - url) : strlen(url);
    const size_t fragment_length = fragment ? strlen(fragment) : 0;

    char *out = static_cast<char *>(malloc(url_prefix_length + base_query_length + fragment_length + 1));
    if (!out) return nullptr;
    memcpy(out, url, url_prefix_length);
    memcpy(out + url_prefix_length, base_query, base_query_length);
    if (fragment_length > 0) {
        memcpy(out + url_prefix_length + base_query_length, fragment, fragment_length);
    }
    out[url_prefix_length + base_query_length + fragment_length] = '\0';
    return out;
}

int64_t FFmpegHlsNetworkUtils::parseM3u8TotalDurationUs(const char *m3u8_content, int *is_vod_out) {
    if (is_vod_out) *is_vod_out = 0;
    if (!m3u8_content || !*m3u8_content) return 0;

    int is_vod = 0;
    double total_seconds = 0.0;

    const char *cursor = m3u8_content;
    while (*cursor) {
        const char *line = cursor;
        const char *end = strchr(cursor, '\n');
        size_t length = end ? static_cast<size_t>(end - line) : strlen(line);
        cursor = end ? (end + 1) : (line + length);

        while (length > 0 && (line[0] == '\r' || isspace(static_cast<unsigned char>(line[0])))) {
            ++line;
            --length;
        }
        while (length > 0 && (line[length - 1] == '\r' || isspace(static_cast<unsigned char>(line[length - 1])))) {
            --length;
        }
        if (length == 0) continue;

        if (length >= 13 && memcmp(line, "#EXT-X-ENDLIST", 13) == 0) {
            is_vod = 1;
            continue;
        }
        if (length >= 24 && memcmp(line, "#EXT-X-PLAYLIST-TYPE:", 22) == 0) {
            if (spanContainsStr0(line, length, "VOD")) is_vod = 1;
            continue;
        }
        if (length >= 8 && memcmp(line, "#EXTINF:", 8) == 0) {
            errno = 0;
            char *number_end = nullptr;
            const double seconds = strtod(line + 8, &number_end);
            if (number_end != line + 8 && errno == 0 && seconds > 0.0) {
                total_seconds += seconds;
            }
        }
    }

    if (is_vod_out) *is_vod_out = is_vod;
    if (total_seconds <= 0.0) return 0;

    const double duration_us = total_seconds * 1000000.0;
    if (duration_us >= static_cast<double>(INT64_MAX)) return INT64_MAX;
    return static_cast<int64_t>(duration_us + 0.5);
}

FFmpegResolvedInputUrl FFmpegHlsNetworkUtils::resolveEffectiveInputUrl(const char *m3u8_url) {
    FFmpegResolvedInputUrl resolved;
    resolved.full_url = m3u8_url ? m3u8_url : "";

    if (!m3u8_url || !*m3u8_url) return resolved;
    if (!urlIsM3u8(m3u8_url)) return resolved;

    char *master_m3u8 = readUrlToString(resolved.full_url.c_str());
    if (!master_m3u8) return resolved;

    char *sub_url_raw = extractSubM3u8Url(master_m3u8, "720p");
    if (!sub_url_raw) {
        sub_url_raw = extractVariantM3u8UriFromMaster(master_m3u8, 720);
    }

    if (sub_url_raw) {
        char *resolved_url = resolveRelativeUrl(m3u8_url, sub_url_raw);
        if (resolved_url) {
            char *with_query = inheritQueryIfMissing(m3u8_url, resolved_url);
            if (with_query) {
                free(resolved_url);
                resolved_url = with_query;
            }
            resolved.full_url = resolved_url;
            AWESOME_FF_LOGI("Found sub m3u8 URL (full): %s", resolved.full_url.c_str());
            free(resolved_url);
        } else {
            AWESOME_FF_LOGI("Found sub m3u8 URL (raw): %s", sub_url_raw);
        }
        free(sub_url_raw);
    } else {
        AWESOME_FF_LOGW("No sub m3u8 URL found for resolution %s", "720p");
    }

    if (resolved.full_url == m3u8_url) {
        resolved.playlist_duration_us = parseM3u8TotalDurationUs(master_m3u8, &resolved.playlist_is_vod);
    } else {
        char *media_m3u8 = readUrlToString(resolved.full_url.c_str());
        if (media_m3u8) {
            resolved.playlist_duration_us = parseM3u8TotalDurationUs(media_m3u8, &resolved.playlist_is_vod);
            free(media_m3u8);
        }
    }
    AWESOME_FF_LOGI("Parsed playlist duration(us)=%" PRId64 ", is_vod=%d",
        resolved.playlist_duration_us,
        resolved.playlist_is_vod
    );

    free(master_m3u8);
    return resolved;
}
