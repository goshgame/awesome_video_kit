#include "ffmpeg_av_file_info_c_api.h"

#include "ffmpeg_av_file_info.h"

#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>

namespace {

constexpr int kFFmpegAvFileInfoInvalidArguments = 1;
constexpr int kFFmpegAvFileInfoLoadFailed = 2;

char *copyCString(const std::string &value) {
    char *result = static_cast<char *>(std::malloc(value.size() + 1));
    if (!result) return nullptr;
    std::memcpy(result, value.c_str(), value.size() + 1);
    return result;
}

std::string jsonEscape(const std::string &value) {
    std::ostringstream escaped;
    for (unsigned char ch : value) {
        switch (ch) {
            case '\\':
                escaped << "\\\\";
                break;
            case '"':
                escaped << "\\\"";
                break;
            case '\b':
                escaped << "\\b";
                break;
            case '\f':
                escaped << "\\f";
                break;
            case '\n':
                escaped << "\\n";
                break;
            case '\r':
                escaped << "\\r";
                break;
            case '\t':
                escaped << "\\t";
                break;
            default:
                if (ch < 0x20) {
                    escaped << "\\u"
                            << std::hex
                            << std::setw(4)
                            << std::setfill('0')
                            << static_cast<int>(ch)
                            << std::dec
                            << std::setfill(' ');
                } else {
                    escaped << static_cast<char>(ch);
                }
                break;
        }
    }
    return escaped.str();
}

std::string safeString(const char *value) {
    return value ? value : "";
}

void appendSize(std::ostringstream &json, const FFmpegAVFileInfo::Size &size) {
    json << "{\"width\":" << size.width << ",\"height\":" << size.height << "}";
}

void appendRational(std::ostringstream &json, const FFmpegAVFileInfo::Rational &rational) {
    json << "{\"num\":" << rational.num << ",\"den\":" << rational.den << "}";
}

std::string buildJson(const FFmpegAVFileInfo &info) {
    std::ostringstream json;
    json << "{";
    json << "\"fileType\":" << static_cast<int>(info.avFileType()) << ",";
    json << "\"duration\":" << info.duration() << ",";
    json << "\"dataRate\":" << info.dataRate() << ",";
    json << "\"videoStreamCount\":" << info.videoStreamCount() << ",";
    json << "\"audioStreamCount\":" << info.audioStreamCount() << ",";
    json << "\"sourcePath\":\"" << jsonEscape(info.sourcePath()) << "\",";

    json << "\"videoStreams\":[";
    for (unsigned int index = 0; index < info.videoStreamCount(); ++index) {
        if (index > 0) json << ",";
        json << "{";
        json << "\"duration\":" << info.getVideoStreamDuration(index) << ",";
        json << "\"dimension\":";
        appendSize(json, info.getVideoStreamDimension(index));
        json << ",";
        json << "\"pixelAspectRatio\":";
        appendRational(json, info.getVideoStreamPixelAspectRatio(index));
        json << ",";
        json << "\"frameRate\":";
        appendRational(json, info.getVideoStreamFrameRate(index));
        json << ",";
        json << "\"rotation\":" << static_cast<int>(info.getVideoStreamRotation(index)) << ",";
        json << "\"rotationDegrees\":" << info.getVideoStreamRotationDegrees(index) << ",";
        json << "\"componentBitCount\":" << info.getVideoStreamComponentBitCount(index) << ",";
        json << "\"codecType\":" << static_cast<int>(info.getVideoStreamCodecType(index)) << ",";
        json << "\"codecName\":\"" << jsonEscape(safeString(info.getVideoStreamCodecName(index))) << "\",";
        json << "\"codecProfile\":" << info.getVideoCodecProfile(index) << ",";
        json << "\"codecLevel\":" << info.getVideoCodecLevel(index) << ",";
        json << "\"colorTransfer\":" << static_cast<int>(info.getVideoStreamColorTransfer(index)) << ",";
        json << "\"hdrType\":" << static_cast<int>(info.getVideoStreamHDRType(index));
        json << "}";
    }
    json << "],";

    json << "\"audioStreams\":[";
    for (unsigned int index = 0; index < info.audioStreamCount(); ++index) {
        if (index > 0) json << ",";
        json << "{";
        json << "\"duration\":" << info.getAudioStreamDuration(index) << ",";
        json << "\"sampleRate\":" << info.getAudioStreamSampleRate(index) << ",";
        json << "\"channelCount\":" << info.getAudioStreamChannelCount(index) << ",";
        json << "\"codecSupported\":" << (info.getAudioStreamCodecSupport(index) ? "true" : "false") << ",";
        json << "\"codecName\":\"" << jsonEscape(safeString(info.getAudioStreamCodecName(index))) << "\"";
        json << "}";
    }
    json << "]";
    json << "}";
    return json.str();
}

}  // namespace

extern "C" {

int ffmpeg_av_file_info_inspect_json(
    const char *file_path,
    char **out_json,
    char **out_error_message,
    int *out_ffmpeg_error_code
) {
    if (out_json) *out_json = nullptr;
    if (out_error_message) *out_error_message = nullptr;
    if (out_ffmpeg_error_code) *out_ffmpeg_error_code = 0;

    if (!file_path || !file_path[0]) {
        if (out_error_message) {
            *out_error_message = copyCString("filePath is required.");
        }
        return kFFmpegAvFileInfoInvalidArguments;
    }

    FFmpegAVFileInfo info;
    const int ret = info.loadFromFile(file_path);
    if (ret < 0) {
        if (out_error_message) {
            *out_error_message = copyCString("Failed to load media info.");
        }
        if (out_ffmpeg_error_code) *out_ffmpeg_error_code = ret;
        return kFFmpegAvFileInfoLoadFailed;
    }

    if (out_json) {
        *out_json = copyCString(buildJson(info));
        if (!*out_json) {
            if (out_error_message) {
                *out_error_message = copyCString("Failed to allocate media info JSON.");
            }
            return kFFmpegAvFileInfoLoadFailed;
        }
    }

    return 0;
}

void ffmpeg_av_file_info_free_string(char *value) {
    std::free(value);
}

}  // extern "C"
