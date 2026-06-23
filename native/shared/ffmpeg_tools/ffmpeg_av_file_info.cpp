#include "ffmpeg_av_file_info.h"

#include "ffmpeg_common_utils.h"
#include "ffmpeg_hls_network.h"

#include <cmath>

extern "C" {
#include <libavutil/display.h>
#include <libavutil/pixdesc.h>
}

namespace {

FFmpegAVFileInfo::Rational makeRational(AVRational rational, AVRational fallback = AVRational{0, 1}) {
    if (rational.num <= 0 || rational.den <= 0) rational = fallback;
    if (rational.num <= 0 || rational.den <= 0) return FFmpegAVFileInfo::Rational{};

    FFmpegAVFileInfo::Rational result;
    result.num = rational.num;
    result.den = rational.den;
    return result;
}

int64_t streamDurationToUs(AVFormatContext *format_context, AVStream *stream) {
    if (!stream) return 0;

    if (stream->duration > 0 && stream->duration != AV_NOPTS_VALUE) {
        return av_rescale_q(stream->duration, stream->time_base, AV_TIME_BASE_Q);
    }

    if (stream->codecpar && stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        AVRational frame_rate = av_guess_frame_rate(format_context, stream, nullptr);
        if ((frame_rate.num <= 0 || frame_rate.den <= 0) && stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0) {
            frame_rate = stream->avg_frame_rate;
        }
        if ((frame_rate.num <= 0 || frame_rate.den <= 0) && stream->r_frame_rate.num > 0 && stream->r_frame_rate.den > 0) {
            frame_rate = stream->r_frame_rate;
        }
        if (stream->nb_frames > 0 && frame_rate.num > 0 && frame_rate.den > 0) {
            return av_rescale_q(stream->nb_frames, av_inv_q(frame_rate), AV_TIME_BASE_Q);
        }
    }

    if (format_context && format_context->nb_streams == 1) {
        return FFmpegCommonUtils::getDurationInUsSafe(format_context);
    }
    return 0;
}

const AVPacketSideData *getCodecparSideData(const AVStream *stream, AVPacketSideDataType type) {
    if (!stream || !stream->codecpar) return nullptr;
    return av_packet_side_data_get(
        stream->codecpar->coded_side_data,
        stream->codecpar->nb_coded_side_data,
        type
    );
}

int normalizeSignedDegrees(int degrees) {
    degrees %= 360;
    if (degrees <= -180) degrees += 360;
    if (degrees > 180) degrees -= 360;
    return degrees;
}

int normalizePositiveDegrees(int degrees) {
    degrees %= 360;
    if (degrees < 0) degrees += 360;
    return degrees;
}

int getStreamRotationDegrees(const AVStream *stream) {
    const AVPacketSideData *display_matrix = getCodecparSideData(stream, AV_PKT_DATA_DISPLAYMATRIX);
    if (display_matrix && display_matrix->data && display_matrix->size >= static_cast<int>(9 * sizeof(int32_t))) {
        const double rotation_ccw = av_display_rotation_get(reinterpret_cast<const int32_t *>(display_matrix->data));
        if (rotation_ccw == rotation_ccw) {
            return normalizeSignedDegrees(static_cast<int>(lround(rotation_ccw)));
        }
    }

    if (stream) {
        AVDictionaryEntry *rotation_entry = av_dict_get(stream->metadata, "rotate", nullptr, 0);
        if (rotation_entry && rotation_entry->value && rotation_entry->value[0]) {
            char *end = nullptr;
            const long rotation = strtol(rotation_entry->value, &end, 10);
            if (end && *end == '\0') {
                return normalizeSignedDegrees(static_cast<int>(rotation));
            }
        }
    }

    return 0;
}

FFmpegAVFileInfo::VideoRotation mapVideoRotation(int rotation_degrees) {
    switch (normalizePositiveDegrees(rotation_degrees)) {
        case 0:
            return FFmpegAVFileInfo::VideoRotation0;
        case 90:
            return FFmpegAVFileInfo::VideoRotation90;
        case 180:
            return FFmpegAVFileInfo::VideoRotation180;
        case 270:
            return FFmpegAVFileInfo::VideoRotation270;
        default:
            return FFmpegAVFileInfo::VideoRotationUnknown;
    }
}

bool isStillImageCodec(AVCodecID codec_id) {
    switch (codec_id) {
        case AV_CODEC_ID_MJPEG:
        case AV_CODEC_ID_LJPEG:
        case AV_CODEC_ID_JPEGLS:
        case AV_CODEC_ID_PNG:
        case AV_CODEC_ID_BMP:
        case AV_CODEC_ID_GIF:
        case AV_CODEC_ID_TIFF:
        case AV_CODEC_ID_WEBP:
        case AV_CODEC_ID_JPEG2000:
        case AV_CODEC_ID_JPEGXL:
        case AV_CODEC_ID_PPM:
        case AV_CODEC_ID_PGM:
        case AV_CODEC_ID_PGMYUV:
        case AV_CODEC_ID_PBM:
        case AV_CODEC_ID_PAM:
        case AV_CODEC_ID_PCX:
        case AV_CODEC_ID_SGI:
        case AV_CODEC_ID_SVG:
            return true;
        default:
            return false;
    }
}

bool formatNameLooksLikeImage(const AVInputFormat *input_format) {
    if (!input_format || !input_format->name) return false;

    const char *name = input_format->name;
    return strstr(name, "image2") ||
        strstr(name, "png_pipe") ||
        strstr(name, "jpeg_pipe") ||
        strstr(name, "webp_pipe") ||
        strstr(name, "gif");
}

bool schemeEqualsCi(const char *scheme, size_t scheme_length, const char *expected) {
    if (!scheme || !expected) return false;
    if (scheme_length != strlen(expected)) return false;

    for (size_t index = 0; index < scheme_length; ++index) {
        if (tolower(static_cast<unsigned char>(scheme[index])) !=
            tolower(static_cast<unsigned char>(expected[index]))) {
            return false;
        }
    }
    return true;
}

bool isNetworkUrl(const char *path) {
    if (!path || !*path) return false;

    const char *scheme_end = strchr(path, ':');
    if (!scheme_end || scheme_end == path) return false;

    const size_t scheme_length = static_cast<size_t>(scheme_end - path);
    return schemeEqualsCi(path, scheme_length, "http") ||
        schemeEqualsCi(path, scheme_length, "https") ||
        schemeEqualsCi(path, scheme_length, "rtmp") ||
        schemeEqualsCi(path, scheme_length, "rtmps") ||
        schemeEqualsCi(path, scheme_length, "rtsp") ||
        schemeEqualsCi(path, scheme_length, "tcp") ||
        schemeEqualsCi(path, scheme_length, "udp") ||
        schemeEqualsCi(path, scheme_length, "tls");
}

uint16_t readLe16(const uint8_t *data) {
    return static_cast<uint16_t>(data[0] | (data[1] << 8));
}

uint16_t readBe16(const uint8_t *data) {
    return static_cast<uint16_t>((data[0] << 8) | data[1]);
}

uint32_t readLe24(const uint8_t *data) {
    return static_cast<uint32_t>(data[0] | (data[1] << 8) | (data[2] << 16));
}

uint32_t readLe32(const uint8_t *data) {
    return static_cast<uint32_t>(data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24));
}

uint32_t readBe32(const uint8_t *data) {
    return static_cast<uint32_t>((data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3]);
}

bool freadExact(FILE *file, uint8_t *data, size_t size) {
    return file && fread(data, 1, size, file) == size;
}

void setImageCodecInfo(
    FFmpegAVFileInfo::VideoStreamInfo *info,
    FFmpegAVFileInfo::VideoCodecType codec_type,
    const char *codec_name
) {
    if (!info) return;
    info->codec_type = codec_type;
    info->codec_name = codec_name ? codec_name : "";
}

bool probePng(FILE *file, FFmpegAVFileInfo::VideoStreamInfo *info) {
    uint8_t header[33] = {0};
    if (fseek(file, 0, SEEK_SET) != 0 || !freadExact(file, header, sizeof(header))) return false;
    const uint8_t signature[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (memcmp(header, signature, sizeof(signature)) != 0 || memcmp(header + 12, "IHDR", 4) != 0) return false;

    info->dimension.width = static_cast<int>(readBe32(header + 16));
    info->dimension.height = static_cast<int>(readBe32(header + 20));
    info->component_bit_count = header[24];
    setImageCodecInfo(info, FFmpegAVFileInfo::VideoCodecTypePNG, "png");
    return info->dimension.width > 0 && info->dimension.height > 0;
}

bool probeJpeg(FILE *file, FFmpegAVFileInfo::VideoStreamInfo *info) {
    uint8_t marker[2] = {0};
    if (fseek(file, 0, SEEK_SET) != 0 || !freadExact(file, marker, sizeof(marker))) return false;
    if (marker[0] != 0xff || marker[1] != 0xd8) return false;

    while (freadExact(file, marker, sizeof(marker))) {
        while (marker[0] != 0xff) {
            marker[0] = marker[1];
            if (!freadExact(file, marker + 1, 1)) return false;
        }
        while (marker[1] == 0xff) {
            if (!freadExact(file, marker + 1, 1)) return false;
        }

        const uint8_t code = marker[1];
        if (code == 0xd9 || code == 0xda) return false;
        uint8_t length_bytes[2] = {0};
        if (!freadExact(file, length_bytes, sizeof(length_bytes))) return false;
        const uint16_t segment_length = readBe16(length_bytes);
        if (segment_length < 2) return false;

        const bool is_sof =
            (code >= 0xc0 && code <= 0xc3) ||
            (code >= 0xc5 && code <= 0xc7) ||
            (code >= 0xc9 && code <= 0xcb) ||
            (code >= 0xcd && code <= 0xcf);
        if (is_sof) {
            uint8_t sof[5] = {0};
            if (!freadExact(file, sof, sizeof(sof))) return false;
            info->component_bit_count = sof[0];
            info->dimension.height = readBe16(sof + 1);
            info->dimension.width = readBe16(sof + 3);
            setImageCodecInfo(info, FFmpegAVFileInfo::VideoCodecTypeMJPEG, "mjpeg");
            return info->dimension.width > 0 && info->dimension.height > 0;
        }

        if (fseek(file, segment_length - 2, SEEK_CUR) != 0) return false;
    }
    return false;
}

bool probeGif(FILE *file, FFmpegAVFileInfo::VideoStreamInfo *info) {
    uint8_t header[10] = {0};
    if (fseek(file, 0, SEEK_SET) != 0 || !freadExact(file, header, sizeof(header))) return false;
    if (memcmp(header, "GIF87a", 6) != 0 && memcmp(header, "GIF89a", 6) != 0) return false;

    info->dimension.width = readLe16(header + 6);
    info->dimension.height = readLe16(header + 8);
    info->component_bit_count = 8;
    setImageCodecInfo(info, FFmpegAVFileInfo::VideoCodecTypeOther, "gif");
    return info->dimension.width > 0 && info->dimension.height > 0;
}

bool probeBmp(FILE *file, FFmpegAVFileInfo::VideoStreamInfo *info) {
    uint8_t header[30] = {0};
    if (fseek(file, 0, SEEK_SET) != 0 || !freadExact(file, header, sizeof(header))) return false;
    if (header[0] != 'B' || header[1] != 'M') return false;

    const uint32_t dib_header_size = readLe32(header + 14);
    if (dib_header_size < 16) return false;

    int width = 0;
    int height = 0;
    if (dib_header_size == 16) {
        width = readLe16(header + 18);
        height = readLe16(header + 20);
    } else {
        width = static_cast<int>(readLe32(header + 18));
        height = std::abs(static_cast<int>(readLe32(header + 22)));
    }

    info->dimension.width = width;
    info->dimension.height = height;
    info->component_bit_count = readLe16(header + 28);
    setImageCodecInfo(info, FFmpegAVFileInfo::VideoCodecTypeOther, "bmp");
    return info->dimension.width > 0 && info->dimension.height > 0;
}

bool probeWebp(FILE *file, FFmpegAVFileInfo::VideoStreamInfo *info) {
    uint8_t header[30] = {0};
    if (fseek(file, 0, SEEK_SET) != 0 || !freadExact(file, header, sizeof(header))) return false;
    if (memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WEBP", 4) != 0) return false;

    if (memcmp(header + 12, "VP8X", 4) == 0) {
        info->dimension.width = static_cast<int>(readLe24(header + 24) + 1);
        info->dimension.height = static_cast<int>(readLe24(header + 27) + 1);
    } else if (memcmp(header + 12, "VP8L", 4) == 0 && header[20] == 0x2f) {
        const uint32_t bits = readLe32(header + 21);
        info->dimension.width = static_cast<int>((bits & 0x3fff) + 1);
        info->dimension.height = static_cast<int>(((bits >> 14) & 0x3fff) + 1);
    } else if (memcmp(header + 12, "VP8 ", 4) == 0) {
        uint8_t frame_header[10] = {0};
        if (fseek(file, 20, SEEK_SET) != 0 || !freadExact(file, frame_header, sizeof(frame_header))) return false;
        if (frame_header[3] != 0x9d || frame_header[4] != 0x01 || frame_header[5] != 0x2a) return false;
        info->dimension.width = readLe16(frame_header + 6) & 0x3fff;
        info->dimension.height = readLe16(frame_header + 8) & 0x3fff;
    } else {
        return false;
    }

    info->component_bit_count = 8;
    setImageCodecInfo(info, FFmpegAVFileInfo::VideoCodecTypeOther, "webp");
    return info->dimension.width > 0 && info->dimension.height > 0;
}

bool heifBrandMatches(const uint8_t *brand) {
    return memcmp(brand, "heic", 4) == 0 ||
        memcmp(brand, "heix", 4) == 0 ||
        memcmp(brand, "hevc", 4) == 0 ||
        memcmp(brand, "hevx", 4) == 0 ||
        memcmp(brand, "heim", 4) == 0 ||
        memcmp(brand, "heis", 4) == 0 ||
        memcmp(brand, "mif1", 4) == 0 ||
        memcmp(brand, "msf1", 4) == 0;
}

bool avifBrandMatches(const uint8_t *brand) {
    return memcmp(brand, "avif", 4) == 0 || memcmp(brand, "avis", 4) == 0;
}

bool probeHeifOrAvif(FILE *file, FFmpegAVFileInfo::VideoStreamInfo *info) {
    uint8_t header[32] = {0};
    if (fseek(file, 0, SEEK_SET) != 0 || !freadExact(file, header, sizeof(header))) return false;
    if (memcmp(header + 4, "ftyp", 4) != 0) return false;

    bool is_heif = heifBrandMatches(header + 8);
    bool is_avif = avifBrandMatches(header + 8);
    for (size_t offset = 16; offset + 4 <= sizeof(header); offset += 4) {
        is_heif = is_heif || heifBrandMatches(header + offset);
        is_avif = is_avif || avifBrandMatches(header + offset);
    }
    if (!is_heif && !is_avif) return false;

    const size_t max_probe_size = 1024 * 1024;
    std::vector<uint8_t> buffer(max_probe_size);
    if (fseek(file, 0, SEEK_SET) != 0) return false;
    const size_t bytes_read = fread(buffer.data(), 1, buffer.size(), file);

    for (size_t offset = 4; offset + 16 <= bytes_read; ++offset) {
        if (memcmp(buffer.data() + offset, "ispe", 4) != 0) continue;

        const uint32_t box_size = readBe32(buffer.data() + offset - 4);
        if (box_size < 20 || offset + 16 > bytes_read) continue;

        const uint32_t width = readBe32(buffer.data() + offset + 8);
        const uint32_t height = readBe32(buffer.data() + offset + 12);
        if (width == 0 || height == 0 || width > INT_MAX || height > INT_MAX) continue;

        info->dimension.width = static_cast<int>(width);
        info->dimension.height = static_cast<int>(height);
        info->component_bit_count = 8;
        setImageCodecInfo(info, FFmpegAVFileInfo::VideoCodecTypeOther, is_avif ? "avif" : "heif");
        return true;
    }
    return false;
}

bool probePngBytes(const uint8_t *data, size_t size, FFmpegAVFileInfo::VideoStreamInfo *info) {
    if (!data || size < 33 || !info) return false;
    const uint8_t signature[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (memcmp(data, signature, sizeof(signature)) != 0 || memcmp(data + 12, "IHDR", 4) != 0) return false;

    info->dimension.width = static_cast<int>(readBe32(data + 16));
    info->dimension.height = static_cast<int>(readBe32(data + 20));
    info->component_bit_count = data[24];
    setImageCodecInfo(info, FFmpegAVFileInfo::VideoCodecTypePNG, "png");
    return info->dimension.width > 0 && info->dimension.height > 0;
}

bool probeJpegBytes(const uint8_t *data, size_t size, FFmpegAVFileInfo::VideoStreamInfo *info) {
    if (!data || size < 4 || !info) return false;
    if (data[0] != 0xff || data[1] != 0xd8) return false;

    size_t offset = 2;
    while (offset + 4 <= size) {
        while (offset < size && data[offset] != 0xff) ++offset;
        while (offset < size && data[offset] == 0xff) ++offset;
        if (offset >= size) return false;

        const uint8_t code = data[offset++];
        if (code == 0xd9 || code == 0xda) return false;
        if (offset + 2 > size) return false;

        const uint16_t segment_length = readBe16(data + offset);
        offset += 2;
        if (segment_length < 2) return false;
        const size_t payload_length = static_cast<size_t>(segment_length - 2);
        if (offset + payload_length > size) return false;

        const bool is_sof =
            (code >= 0xc0 && code <= 0xc3) ||
            (code >= 0xc5 && code <= 0xc7) ||
            (code >= 0xc9 && code <= 0xcb) ||
            (code >= 0xcd && code <= 0xcf);
        if (is_sof) {
            if (payload_length < 5) return false;
            info->component_bit_count = data[offset];
            info->dimension.height = readBe16(data + offset + 1);
            info->dimension.width = readBe16(data + offset + 3);
            setImageCodecInfo(info, FFmpegAVFileInfo::VideoCodecTypeMJPEG, "mjpeg");
            return info->dimension.width > 0 && info->dimension.height > 0;
        }

        offset += payload_length;
    }
    return false;
}

bool probeGifBytes(const uint8_t *data, size_t size, FFmpegAVFileInfo::VideoStreamInfo *info) {
    if (!data || size < 10 || !info) return false;
    if (memcmp(data, "GIF87a", 6) != 0 && memcmp(data, "GIF89a", 6) != 0) return false;

    info->dimension.width = readLe16(data + 6);
    info->dimension.height = readLe16(data + 8);
    info->component_bit_count = 8;
    setImageCodecInfo(info, FFmpegAVFileInfo::VideoCodecTypeOther, "gif");
    return info->dimension.width > 0 && info->dimension.height > 0;
}

bool probeBmpBytes(const uint8_t *data, size_t size, FFmpegAVFileInfo::VideoStreamInfo *info) {
    if (!data || size < 30 || !info) return false;
    if (data[0] != 'B' || data[1] != 'M') return false;

    const uint32_t dib_header_size = readLe32(data + 14);
    if (dib_header_size < 16) return false;

    int width = 0;
    int height = 0;
    if (dib_header_size == 16) {
        width = readLe16(data + 18);
        height = readLe16(data + 20);
    } else {
        width = static_cast<int>(readLe32(data + 18));
        height = std::abs(static_cast<int>(readLe32(data + 22)));
    }

    info->dimension.width = width;
    info->dimension.height = height;
    info->component_bit_count = readLe16(data + 28);
    setImageCodecInfo(info, FFmpegAVFileInfo::VideoCodecTypeOther, "bmp");
    return info->dimension.width > 0 && info->dimension.height > 0;
}

bool probeWebpBytes(const uint8_t *data, size_t size, FFmpegAVFileInfo::VideoStreamInfo *info) {
    if (!data || size < 30 || !info) return false;
    if (memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WEBP", 4) != 0) return false;

    if (memcmp(data + 12, "VP8X", 4) == 0) {
        info->dimension.width = static_cast<int>(readLe24(data + 24) + 1);
        info->dimension.height = static_cast<int>(readLe24(data + 27) + 1);
    } else if (memcmp(data + 12, "VP8L", 4) == 0 && size >= 25 && data[20] == 0x2f) {
        const uint32_t bits = readLe32(data + 21);
        info->dimension.width = static_cast<int>((bits & 0x3fff) + 1);
        info->dimension.height = static_cast<int>(((bits >> 14) & 0x3fff) + 1);
    } else if (memcmp(data + 12, "VP8 ", 4) == 0) {
        if (size < 30) return false;
        const uint8_t *frame_header = data + 20;
        if (frame_header[3] != 0x9d || frame_header[4] != 0x01 || frame_header[5] != 0x2a) return false;
        info->dimension.width = readLe16(frame_header + 6) & 0x3fff;
        info->dimension.height = readLe16(frame_header + 8) & 0x3fff;
    } else {
        return false;
    }

    info->component_bit_count = 8;
    setImageCodecInfo(info, FFmpegAVFileInfo::VideoCodecTypeOther, "webp");
    return info->dimension.width > 0 && info->dimension.height > 0;
}

bool probeHeifOrAvifBytes(const uint8_t *data, size_t size, FFmpegAVFileInfo::VideoStreamInfo *info) {
    if (!data || size < 32 || !info) return false;
    if (memcmp(data + 4, "ftyp", 4) != 0) return false;

    bool is_heif = heifBrandMatches(data + 8);
    bool is_avif = avifBrandMatches(data + 8);
    for (size_t offset = 16; offset + 4 <= 32; offset += 4) {
        is_heif = is_heif || heifBrandMatches(data + offset);
        is_avif = is_avif || avifBrandMatches(data + offset);
    }
    if (!is_heif && !is_avif) return false;

    for (size_t offset = 4; offset + 16 <= size; ++offset) {
        if (memcmp(data + offset, "ispe", 4) != 0) continue;

        const uint32_t box_size = readBe32(data + offset - 4);
        if (box_size < 20 || offset + 16 > size) continue;

        const uint32_t width = readBe32(data + offset + 8);
        const uint32_t height = readBe32(data + offset + 12);
        if (width == 0 || height == 0 || width > INT_MAX || height > INT_MAX) continue;

        info->dimension.width = static_cast<int>(width);
        info->dimension.height = static_cast<int>(height);
        info->component_bit_count = 8;
        setImageCodecInfo(info, FFmpegAVFileInfo::VideoCodecTypeOther, is_avif ? "avif" : "heif");
        return true;
    }
    return false;
}

bool probeImageHeaderBytes(const uint8_t *data, size_t size, FFmpegAVFileInfo::VideoStreamInfo *info) {
    if (!data || size == 0 || !info) return false;

    FFmpegAVFileInfo::VideoStreamInfo image_info;
    const bool ok =
        probePngBytes(data, size, &image_info) ||
        probeJpegBytes(data, size, &image_info) ||
        probeGifBytes(data, size, &image_info) ||
        probeWebpBytes(data, size, &image_info) ||
        probeBmpBytes(data, size, &image_info) ||
        probeHeifOrAvifBytes(data, size, &image_info);
    if (!ok) return false;
    *info = image_info;
    return true;
}

bool probeImageHeader(const char *file_path, FFmpegAVFileInfo::VideoStreamInfo *info) {
    if (!file_path || !file_path[0] || !info) return false;

    FILE *file = fopen(file_path, "rb");
    if (!file) return false;

    FFmpegAVFileInfo::VideoStreamInfo image_info;
    const bool ok =
        probePng(file, &image_info) ||
        probeJpeg(file, &image_info) ||
        probeGif(file, &image_info) ||
        probeWebp(file, &image_info) ||
        probeBmp(file, &image_info) ||
        probeHeifOrAvif(file, &image_info);
    fclose(file);

    if (!ok) return false;
    *info = image_info;
    return true;
}

bool probeNetworkImageHeader(const char *url, FFmpegAVFileInfo::VideoStreamInfo *info) {
    if (!url || !url[0] || !info) return false;

    AVIOContext *io_context = nullptr;
    AVDictionary *options = nullptr;
    FFmpegHlsNetworkUtils::setCommonNetworkOptions(&options);

    const int open_ret = avio_open2(&io_context, url, AVIO_FLAG_READ, nullptr, &options);
    av_dict_free(&options);
    if (open_ret < 0) return false;

    constexpr size_t kMaxNetworkImageProbeSize = 1024 * 1024;
    constexpr size_t kReadChunkSize = 16 * 1024;
    std::vector<uint8_t> buffer;
    buffer.reserve(kReadChunkSize);

    uint8_t chunk[kReadChunkSize];
    while (buffer.size() < kMaxNetworkImageProbeSize) {
        const size_t remaining = kMaxNetworkImageProbeSize - buffer.size();
        const int read_size = static_cast<int>(remaining < kReadChunkSize ? remaining : kReadChunkSize);
        const int bytes_read = avio_read(io_context, chunk, read_size);
        if (bytes_read == 0 || bytes_read == AVERROR_EOF) break;
        if (bytes_read < 0) {
            avio_closep(&io_context);
            return false;
        }
        buffer.insert(buffer.end(), chunk, chunk + bytes_read);

        if (probeImageHeaderBytes(buffer.data(), buffer.size(), info)) {
            avio_closep(&io_context);
            return true;
        }
    }

    avio_closep(&io_context);
    return probeImageHeaderBytes(buffer.data(), buffer.size(), info);
}

bool formatContextLooksLikeImage(AVFormatContext *format_context) {
    if (!format_context) return false;

    unsigned int video_stream_count = 0;
    unsigned int audio_stream_count = 0;
    AVCodecID first_video_codec_id = AV_CODEC_ID_NONE;
    for (unsigned int index = 0; index < format_context->nb_streams; ++index) {
        AVStream *stream = format_context->streams[index];
        if (!stream || !stream->codecpar) continue;

        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            ++video_stream_count;
            if (first_video_codec_id == AV_CODEC_ID_NONE) {
                first_video_codec_id = stream->codecpar->codec_id;
            }
        } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            ++audio_stream_count;
        }
    }

    return video_stream_count == 1 &&
        audio_stream_count == 0 &&
        (formatNameLooksLikeImage(format_context->iformat) || isStillImageCodec(first_video_codec_id));
}

int openInputWithOptions(
    AVFormatContext **format_context,
    const char *file_path,
    const AVInputFormat *input_format,
    bool network_input
) {
    AVDictionary *options = nullptr;
    if (network_input) {
        FFmpegHlsNetworkUtils::setCommonNetworkOptions(&options);
    }

    const int ret = avformat_open_input(format_context, file_path, input_format, &options);
    av_dict_free(&options);
    return ret;
}

int openInputWithImageFallback(AVFormatContext **format_context, const char *file_path, bool network_input) {
    int ret = openInputWithOptions(format_context, file_path, nullptr, network_input);
    if (ret >= 0) return ret;

    const AVInputFormat *image_input_format = av_find_input_format("image2");
    if (!image_input_format) return ret;

    AVFormatContext *image_format_context = nullptr;
    const int image_ret = openInputWithOptions(&image_format_context, file_path, image_input_format, network_input);
    if (image_ret < 0) return ret;

    *format_context = image_format_context;
    return image_ret;
}

FFmpegAVFileInfo::VideoCodecType mapVideoCodecType(AVCodecID codec_id) {
    switch (codec_id) {
        case AV_CODEC_ID_H264:
            return FFmpegAVFileInfo::VideoCodecTypeH264;
        case AV_CODEC_ID_HEVC:
            return FFmpegAVFileInfo::VideoCodecTypeHEVC;
        case AV_CODEC_ID_MPEG4:
            return FFmpegAVFileInfo::VideoCodecTypeMPEG4;
        case AV_CODEC_ID_MPEG2VIDEO:
            return FFmpegAVFileInfo::VideoCodecTypeMPEG2;
        case AV_CODEC_ID_H263:
            return FFmpegAVFileInfo::VideoCodecTypeH263;
        case AV_CODEC_ID_VP8:
            return FFmpegAVFileInfo::VideoCodecTypeVP8;
        case AV_CODEC_ID_VP9:
            return FFmpegAVFileInfo::VideoCodecTypeVP9;
        case AV_CODEC_ID_AV1:
            return FFmpegAVFileInfo::VideoCodecTypeAV1;
        case AV_CODEC_ID_MJPEG:
            return FFmpegAVFileInfo::VideoCodecTypeMJPEG;
        case AV_CODEC_ID_PNG:
            return FFmpegAVFileInfo::VideoCodecTypePNG;
        case AV_CODEC_ID_PRORES:
            return FFmpegAVFileInfo::VideoCodecTypeProRes;
        case AV_CODEC_ID_DNXHD:
            return FFmpegAVFileInfo::VideoCodecTypeDNxHD;
        case AV_CODEC_ID_VC1:
            return FFmpegAVFileInfo::VideoCodecTypeVC1;
        case AV_CODEC_ID_WMV3:
            return FFmpegAVFileInfo::VideoCodecTypeWMV3;
        case AV_CODEC_ID_NONE:
            return FFmpegAVFileInfo::VideoCodecTypeUnknown;
        default:
            return FFmpegAVFileInfo::VideoCodecTypeOther;
    }
}

FFmpegAVFileInfo::VideoColorTransfer mapVideoColorTransfer(AVColorTransferCharacteristic trc) {
    switch (trc) {
        case AVCOL_TRC_BT709:
            return FFmpegAVFileInfo::VideoColorTransferBT709;
        case AVCOL_TRC_GAMMA22:
            return FFmpegAVFileInfo::VideoColorTransferGamma22;
        case AVCOL_TRC_GAMMA28:
            return FFmpegAVFileInfo::VideoColorTransferGamma28;
        case AVCOL_TRC_SMPTE170M:
            return FFmpegAVFileInfo::VideoColorTransferSMPTE170M;
        case AVCOL_TRC_SMPTE240M:
            return FFmpegAVFileInfo::VideoColorTransferSMPTE240M;
        case AVCOL_TRC_LINEAR:
            return FFmpegAVFileInfo::VideoColorTransferLinear;
        case AVCOL_TRC_LOG:
            return FFmpegAVFileInfo::VideoColorTransferLog;
        case AVCOL_TRC_LOG_SQRT:
            return FFmpegAVFileInfo::VideoColorTransferLogSqrt;
        case AVCOL_TRC_IEC61966_2_4:
            return FFmpegAVFileInfo::VideoColorTransferIEC61966_2_4;
        case AVCOL_TRC_BT1361_ECG:
            return FFmpegAVFileInfo::VideoColorTransferBT1361ECG;
        case AVCOL_TRC_IEC61966_2_1:
            return FFmpegAVFileInfo::VideoColorTransferSRGB;
        case AVCOL_TRC_BT2020_10:
            return FFmpegAVFileInfo::VideoColorTransferBT2020_10;
        case AVCOL_TRC_BT2020_12:
            return FFmpegAVFileInfo::VideoColorTransferBT2020_12;
        case AVCOL_TRC_SMPTE2084:
            return FFmpegAVFileInfo::VideoColorTransferPQ;
        case AVCOL_TRC_ARIB_STD_B67:
            return FFmpegAVFileInfo::VideoColorTransferHLG;
        case AVCOL_TRC_UNSPECIFIED:
        case AVCOL_TRC_RESERVED0:
        case AVCOL_TRC_RESERVED:
            return FFmpegAVFileInfo::VideoColorTransferUnknown;
        default:
            return FFmpegAVFileInfo::VideoColorTransferOther;
    }
}

FFmpegAVFileInfo::VideoHDRType detectVideoHDRType(const AVStream *stream) {
    if (!stream || !stream->codecpar) return FFmpegAVFileInfo::VideoHDRTypeUnknown;

    if (getCodecparSideData(stream, AV_PKT_DATA_DOVI_CONF)) {
        return FFmpegAVFileInfo::VideoHDRTypeDolbyVision;
    }
    if (getCodecparSideData(stream, AV_PKT_DATA_DYNAMIC_HDR10_PLUS)) {
        return FFmpegAVFileInfo::VideoHDRTypeHDR10Plus;
    }

    const AVColorTransferCharacteristic trc =
        static_cast<AVColorTransferCharacteristic>(stream->codecpar->color_trc);

    if (trc == AVCOL_TRC_ARIB_STD_B67) {
        return FFmpegAVFileInfo::VideoHDRTypeHLG;
    }
    if (trc == AVCOL_TRC_SMPTE2084) {
        return FFmpegAVFileInfo::VideoHDRTypeHDR10;
    }

    switch (trc) {
        case AVCOL_TRC_BT709:
        case AVCOL_TRC_GAMMA22:
        case AVCOL_TRC_GAMMA28:
        case AVCOL_TRC_SMPTE170M:
        case AVCOL_TRC_SMPTE240M:
        case AVCOL_TRC_LINEAR:
        case AVCOL_TRC_LOG:
        case AVCOL_TRC_LOG_SQRT:
        case AVCOL_TRC_IEC61966_2_4:
        case AVCOL_TRC_BT1361_ECG:
        case AVCOL_TRC_IEC61966_2_1:
        case AVCOL_TRC_BT2020_10:
        case AVCOL_TRC_BT2020_12:
            return FFmpegAVFileInfo::VideoHDRTypeSDR;
        default:
            return FFmpegAVFileInfo::VideoHDRTypeUnknown;
    }
}

unsigned int getComponentBitCount(const AVStream *stream) {
    if (!stream || !stream->codecpar) return 0;

    if (stream->codecpar->bits_per_raw_sample > 0) {
        return static_cast<unsigned int>(stream->codecpar->bits_per_raw_sample);
    }

    const enum AVPixelFormat pixel_format = static_cast<AVPixelFormat>(stream->codecpar->format);
    const AVPixFmtDescriptor *pix_desc = av_pix_fmt_desc_get(pixel_format);
    if (pix_desc && pix_desc->nb_components > 0) {
        return static_cast<unsigned int>(pix_desc->comp[0].depth);
    }

    return 0;
}

uint64_t computeDataRate(AVFormatContext *format_context) {
    if (!format_context) return 0;
    if (format_context->bit_rate > 0) return static_cast<uint64_t>(format_context->bit_rate);

    uint64_t bit_rate = 0;
    for (unsigned int index = 0; index < format_context->nb_streams; ++index) {
        AVStream *stream = format_context->streams[index];
        if (stream && stream->codecpar && stream->codecpar->bit_rate > 0) {
            bit_rate += static_cast<uint64_t>(stream->codecpar->bit_rate);
        }
    }
    return bit_rate;
}

FFmpegAVFileInfo::FileType detectFileType(
    AVFormatContext *format_context,
    const std::vector<FFmpegAVFileInfo::VideoStreamInfo> &video_streams,
    unsigned int audio_stream_count,
    AVCodecID first_video_codec_id
) {
    if (!video_streams.empty()) {
        if (video_streams.size() == 1 && audio_stream_count == 0) {
            const int64_t duration_us = FFmpegCommonUtils::getDurationInUsSafe(format_context);
            if (formatNameLooksLikeImage(format_context ? format_context->iformat : nullptr)) {
                return FFmpegAVFileInfo::FileTypeImage;
            }
            if (isStillImageCodec(first_video_codec_id) && duration_us <= 0) {
                return FFmpegAVFileInfo::FileTypeImage;
            }
        }
        return FFmpegAVFileInfo::FileTypeVideo;
    }

    if (audio_stream_count > 0) return FFmpegAVFileInfo::FileTypeAudio;
    return FFmpegAVFileInfo::FileTypeUnknown;
}

const FFmpegAVFileInfo::VideoStreamInfo *getVideoStreamInfoByIndex(
    const std::vector<FFmpegAVFileInfo::VideoStreamInfo> &video_streams,
    unsigned int index
) {
    return index < video_streams.size() ? &video_streams[index] : nullptr;
}

const FFmpegAVFileInfo::AudioStreamInfo *getAudioStreamInfoByIndex(
    const std::vector<FFmpegAVFileInfo::AudioStreamInfo> &audio_streams,
    unsigned int index
) {
    return index < audio_streams.size() ? &audio_streams[index] : nullptr;
}

}  // namespace

FFmpegAVFileInfo::FFmpegAVFileInfo()
    : source_path_(),
      av_file_type_(FileTypeUnknown),
      duration_(0),
      data_rate_(0),
      video_streams_(),
      audio_streams_() {
    FFmpegCommonUtils::installPlatformLogBridge();
}

void FFmpegAVFileInfo::clear() {
    source_path_.clear();
    av_file_type_ = FileTypeUnknown;
    duration_ = 0;
    data_rate_ = 0;
    video_streams_.clear();
    audio_streams_.clear();
}

int FFmpegAVFileInfo::loadFromFile(const char *file_path) {
    AVFormatContext *format_context = nullptr;
    VideoStreamInfo image_header_info;
    AVCodecID first_video_codec_id = AV_CODEC_ID_NONE;
    int ret = 0;

    clear();
    if (!file_path || !file_path[0]) return AVERROR(EINVAL);

    const bool network_input = isNetworkUrl(file_path);
    if (network_input) {
        avformat_network_init();
    }
    const bool has_image_header_info = network_input
        ? probeNetworkImageHeader(file_path, &image_header_info)
        : probeImageHeader(file_path, &image_header_info);

    ret = openInputWithImageFallback(&format_context, file_path, network_input);
    if (ret < 0) {
        if (has_image_header_info) {
            source_path_ = file_path;
            av_file_type_ = FileTypeImage;
            video_streams_.push_back(image_header_info);
            ret = 0;
        }
        goto end;
    }

    ret = avformat_find_stream_info(format_context, nullptr);
    if (ret < 0 && !formatContextLooksLikeImage(format_context)) goto end;

    source_path_ = file_path;
    duration_ = FFmpegCommonUtils::getDurationInUsSafe(format_context);
    data_rate_ = computeDataRate(format_context);

    for (unsigned int index = 0; index < format_context->nb_streams; ++index) {
        AVStream *stream = format_context->streams[index];
        if (!stream || !stream->codecpar) continue;

        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            VideoStreamInfo info;
            const AVCodecDescriptor *codec_desc = avcodec_descriptor_get(stream->codecpar->codec_id);
            AVRational frame_rate = av_guess_frame_rate(format_context, stream, nullptr);

            info.duration_us = streamDurationToUs(format_context, stream);
            info.dimension.width = stream->codecpar->width;
            info.dimension.height = stream->codecpar->height;
            if ((info.dimension.width <= 0 || info.dimension.height <= 0) && has_image_header_info) {
                info.dimension = image_header_info.dimension;
            }
            info.pixel_aspect_ratio = makeRational(
                stream->sample_aspect_ratio,
                stream->codecpar->sample_aspect_ratio
            );
            if (frame_rate.num <= 0 || frame_rate.den <= 0) frame_rate = stream->avg_frame_rate;
            if (frame_rate.num <= 0 || frame_rate.den <= 0) frame_rate = stream->r_frame_rate;
            info.frame_rate = makeRational(frame_rate);
            info.rotation_degrees = getStreamRotationDegrees(stream);
            info.rotation = mapVideoRotation(info.rotation_degrees);
            info.component_bit_count = getComponentBitCount(stream);
            if (info.component_bit_count == 0 && has_image_header_info) {
                info.component_bit_count = image_header_info.component_bit_count;
            }
            info.codec_type = mapVideoCodecType(stream->codecpar->codec_id);
            info.codec_name = codec_desc && codec_desc->name ? codec_desc->name : "";
            if (info.codec_type == VideoCodecTypeUnknown && has_image_header_info) {
                info.codec_type = image_header_info.codec_type;
                info.codec_name = image_header_info.codec_name;
            }
            info.codec_profile = stream->codecpar->profile >= 0 ? stream->codecpar->profile : -1;
            info.codec_level = stream->codecpar->level > 0 ? stream->codecpar->level : -1;
            info.color_transfer = mapVideoColorTransfer(
                static_cast<AVColorTransferCharacteristic>(stream->codecpar->color_trc)
            );
            info.hdr_type = detectVideoHDRType(stream);

            if (first_video_codec_id == AV_CODEC_ID_NONE) {
                first_video_codec_id = stream->codecpar->codec_id;
            }
            video_streams_.push_back(info);
        } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            AudioStreamInfo info;
            const AVCodecDescriptor *codec_desc = avcodec_descriptor_get(stream->codecpar->codec_id);

            info.duration_us = streamDurationToUs(format_context, stream);
            info.sample_rate = stream->codecpar->sample_rate > 0
                ? static_cast<unsigned int>(stream->codecpar->sample_rate)
                : 0;
            info.channel_count = stream->codecpar->ch_layout.nb_channels > 0
                ? static_cast<unsigned int>(stream->codecpar->ch_layout.nb_channels)
                : 0;
            info.codec_supported = avcodec_find_decoder(stream->codecpar->codec_id) != nullptr;
            info.codec_name = codec_desc && codec_desc->name ? codec_desc->name : "";
            audio_streams_.push_back(info);
        }
    }

    av_file_type_ = detectFileType(format_context, video_streams_, audioStreamCount(), first_video_codec_id);
    if (av_file_type_ == FileTypeUnknown && has_image_header_info && video_streams_.empty() && audio_streams_.empty()) {
        av_file_type_ = FileTypeImage;
        video_streams_.push_back(image_header_info);
    }
    ret = 0;

end:
    if (format_context) avformat_close_input(&format_context);
    if (ret < 0) clear();
    return ret;
}

FFmpegAVFileInfo::FileType FFmpegAVFileInfo::avFileType() const {
    return av_file_type_;
}

int64_t FFmpegAVFileInfo::duration() const {
    return duration_;
}

uint64_t FFmpegAVFileInfo::dataRate() const {
    return data_rate_;
}

unsigned int FFmpegAVFileInfo::videoStreamCount() const {
    return static_cast<unsigned int>(video_streams_.size());
}

unsigned int FFmpegAVFileInfo::audioStreamCount() const {
    return static_cast<unsigned int>(audio_streams_.size());
}

const std::string &FFmpegAVFileInfo::sourcePath() const {
    return source_path_;
}

int64_t FFmpegAVFileInfo::getVideoStreamDuration(unsigned int video_stream_index) const {
    const VideoStreamInfo *info = getVideoStreamInfo(video_stream_index);
    return info ? info->duration_us : 0;
}

FFmpegAVFileInfo::Size FFmpegAVFileInfo::getVideoStreamDimension(unsigned int video_stream_index) const {
    const VideoStreamInfo *info = getVideoStreamInfo(video_stream_index);
    return info ? info->dimension : Size{};
}

FFmpegAVFileInfo::Rational FFmpegAVFileInfo::getVideoStreamPixelAspectRatio(unsigned int video_stream_index) const {
    const VideoStreamInfo *info = getVideoStreamInfo(video_stream_index);
    return info ? info->pixel_aspect_ratio : Rational{};
}

FFmpegAVFileInfo::Rational FFmpegAVFileInfo::getVideoStreamFrameRate(unsigned int video_stream_index) const {
    const VideoStreamInfo *info = getVideoStreamInfo(video_stream_index);
    return info ? info->frame_rate : Rational{};
}

FFmpegAVFileInfo::VideoRotation FFmpegAVFileInfo::getVideoStreamRotation(unsigned int video_stream_index) const {
    const VideoStreamInfo *info = getVideoStreamInfo(video_stream_index);
    return info ? info->rotation : VideoRotationUnknown;
}

int FFmpegAVFileInfo::getVideoStreamRotationDegrees(unsigned int video_stream_index) const {
    const VideoStreamInfo *info = getVideoStreamInfo(video_stream_index);
    return info ? info->rotation_degrees : 0;
}

unsigned int FFmpegAVFileInfo::getVideoStreamComponentBitCount(unsigned int video_stream_index) const {
    const VideoStreamInfo *info = getVideoStreamInfo(video_stream_index);
    return info ? info->component_bit_count : 0;
}

FFmpegAVFileInfo::VideoCodecType FFmpegAVFileInfo::getVideoStreamCodecType(unsigned int video_stream_index) const {
    const VideoStreamInfo *info = getVideoStreamInfo(video_stream_index);
    return info ? info->codec_type : VideoCodecTypeUnknown;
}

const char *FFmpegAVFileInfo::getVideoStreamCodecName(unsigned int video_stream_index) const {
    const VideoStreamInfo *info = getVideoStreamInfo(video_stream_index);
    return info ? info->codec_name.c_str() : "";
}

int FFmpegAVFileInfo::getVideoCodecProfile(unsigned int video_stream_index) const {
    const VideoStreamInfo *info = getVideoStreamInfo(video_stream_index);
    return info ? info->codec_profile : -1;
}

int FFmpegAVFileInfo::getVideoCodecLevel(unsigned int video_stream_index) const {
    const VideoStreamInfo *info = getVideoStreamInfo(video_stream_index);
    return info ? info->codec_level : -1;
}

FFmpegAVFileInfo::VideoColorTransfer FFmpegAVFileInfo::getVideoStreamColorTransfer(unsigned int video_stream_index) const {
    const VideoStreamInfo *info = getVideoStreamInfo(video_stream_index);
    return info ? info->color_transfer : VideoColorTransferUnknown;
}

FFmpegAVFileInfo::VideoHDRType FFmpegAVFileInfo::getVideoStreamHDRType(unsigned int video_stream_index) const {
    const VideoStreamInfo *info = getVideoStreamInfo(video_stream_index);
    return info ? info->hdr_type : VideoHDRTypeUnknown;
}

int64_t FFmpegAVFileInfo::getAudioStreamDuration(unsigned int audio_stream_index) const {
    const AudioStreamInfo *info = getAudioStreamInfo(audio_stream_index);
    return info ? info->duration_us : 0;
}

unsigned int FFmpegAVFileInfo::getAudioStreamSampleRate(unsigned int audio_stream_index) const {
    const AudioStreamInfo *info = getAudioStreamInfo(audio_stream_index);
    return info ? info->sample_rate : 0;
}

unsigned int FFmpegAVFileInfo::getAudioStreamChannelCount(unsigned int audio_stream_index) const {
    const AudioStreamInfo *info = getAudioStreamInfo(audio_stream_index);
    return info ? info->channel_count : 0;
}

bool FFmpegAVFileInfo::getAudioStreamCodecSupport(unsigned int audio_stream_index) const {
    const AudioStreamInfo *info = getAudioStreamInfo(audio_stream_index);
    return info ? info->codec_supported : false;
}

const char *FFmpegAVFileInfo::getAudioStreamCodecName(unsigned int audio_stream_index) const {
    const AudioStreamInfo *info = getAudioStreamInfo(audio_stream_index);
    return info ? info->codec_name.c_str() : "";
}

const FFmpegAVFileInfo::VideoStreamInfo *FFmpegAVFileInfo::getVideoStreamInfo(unsigned int video_stream_index) const {
    return getVideoStreamInfoByIndex(video_streams_, video_stream_index);
}

const FFmpegAVFileInfo::AudioStreamInfo *FFmpegAVFileInfo::getAudioStreamInfo(unsigned int audio_stream_index) const {
    return getAudioStreamInfoByIndex(audio_streams_, audio_stream_index);
}
