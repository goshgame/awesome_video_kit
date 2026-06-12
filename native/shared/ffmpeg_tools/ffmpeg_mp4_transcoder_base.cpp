#include "ffmpeg_mp4_transcoder_base.h"

#include "ffmpeg_common_utils.h"

#include <chrono>
#include <thread>
#include <vector>

namespace {

enum VideoExtradataSource {
    kVideoExtradataSourceNone = 0,
    kVideoExtradataSourcePacketSideData,
    kVideoExtradataSourceEncoderContext,
    kVideoExtradataSourcePacketParameterSets,
};

const char *describeVideoExtradataSource(VideoExtradataSource source) {
    switch (source) {
        case kVideoExtradataSourcePacketSideData:
            return "packet-side-data";
        case kVideoExtradataSourceEncoderContext:
            return "encoder-context";
        case kVideoExtradataSourcePacketParameterSets:
            return "packet-parameter-sets";
        case kVideoExtradataSourceNone:
        default:
            return "none";
    }
}

const char *defaultPreset(const FFmpegMp4TranscodeConfig *config) {
    return config && config->preset && config->preset[0] ? config->preset : "fast";
}

const char *defaultProfile(const FFmpegMp4TranscodeConfig *config) {
    return config && config->profile && config->profile[0] ? config->profile : "high";
}

const char *defaultLevel(const FFmpegMp4TranscodeConfig *config) {
    return config && config->level && config->level[0] ? config->level : "4.2";
}

int defaultCrf(const FFmpegMp4TranscodeConfig *config) {
    return config && config->crf > 0 ? config->crf : 23;
}

int defaultAudioBitrate(const FFmpegMp4TranscodeConfig *config) {
    return config && config->audio_bitrate > 0 ? config->audio_bitrate : 128000;
}

int defaultFaststart(const FFmpegMp4TranscodeConfig *config) {
    return !config || config->faststart >= 0 ? 1 : 0;
}

int defaultFrameRate(const FFmpegMp4TranscodeConfig *config) {
    return config && config->frame_rate > 0 ? config->frame_rate : 0;
}

int defaultScaleWidth(const FFmpegMp4TranscodeConfig *config) {
    return config && config->scale_width > 0 ? config->scale_width : 0;
}

int defaultScaleHeight(const FFmpegMp4TranscodeConfig *config) {
    return config && config->scale_height > 0 ? config->scale_height : 0;
}

AVRational fallbackVideoFrameRate(AVFormatContext *format_context, AVStream *input_stream) {
    AVRational frame_rate = av_guess_frame_rate(format_context, input_stream, nullptr);
    if ((frame_rate.num <= 0 || frame_rate.den <= 0) && input_stream) {
        frame_rate = input_stream->avg_frame_rate;
    }
    if ((frame_rate.num <= 0 || frame_rate.den <= 0) && input_stream) {
        frame_rate = input_stream->r_frame_rate;
    }
    if (frame_rate.num <= 0 || frame_rate.den <= 0) {
        frame_rate = AVRational{25, 1};
    }
    return frame_rate;
}

int64_t packetTimestampAnchor(const AVPacket *packet) {
    if (!packet) return 0;
    if (packet->dts != AV_NOPTS_VALUE) return packet->dts;
    if (packet->pts != AV_NOPTS_VALUE) return packet->pts;
    return 0;
}

void shiftPacketTimestamps(AVPacket *packet, int64_t shift) {
    if (!packet || shift == 0) return;
    if (packet->pts != AV_NOPTS_VALUE) packet->pts += shift;
    if (packet->dts != AV_NOPTS_VALUE) packet->dts += shift;
}

int64_t muxerNextVideoDts(AVStream *stream) {
    if (!stream) return AV_NOPTS_VALUE;
#if FF_API_GET_END_PTS
    return av_stream_get_end_pts(stream);
#else
    return AV_NOPTS_VALUE;
#endif
}

int64_t minimumNextVideoDts(AVStream *stream, int64_t last_written_dts) {
    int64_t minimum_dts = AV_NOPTS_VALUE;
    if (last_written_dts != AV_NOPTS_VALUE) {
        minimum_dts = last_written_dts + 1;
    }

    const int64_t muxer_next_dts = muxerNextVideoDts(stream);
    if (muxer_next_dts != AV_NOPTS_VALUE &&
        (minimum_dts == AV_NOPTS_VALUE || muxer_next_dts > minimum_dts)) {
        minimum_dts = muxer_next_dts;
    }
    return minimum_dts;
}

std::string describeCodecTag(uint32_t codec_tag) {
    if (codec_tag == 0) return "0";

    char buffer[32];
    const uint8_t a = static_cast<uint8_t>(codec_tag & 0xFF);
    const uint8_t b = static_cast<uint8_t>((codec_tag >> 8) & 0xFF);
    const uint8_t c = static_cast<uint8_t>((codec_tag >> 16) & 0xFF);
    const uint8_t d = static_cast<uint8_t>((codec_tag >> 24) & 0xFF);
    snprintf(
        buffer,
        sizeof(buffer),
        "%c%c%c%c/0x%08X",
        a >= 32 && a <= 126 ? a : '.',
        b >= 32 && b <= 126 ? b : '.',
        c >= 32 && c <= 126 ? c : '.',
        d >= 32 && d <= 126 ? d : '.',
        codec_tag
    );
    return std::string(buffer);
}

int h264AvccNalLengthSize(const AVCodecParameters *codecpar) {
    if (!codecpar || !codecpar->extradata || codecpar->extradata_size < 5) return 4;
    if (codecpar->extradata[0] != 1) return 4;
    return (codecpar->extradata[4] & 0x03) + 1;
}

bool packetContainsH264Idr(const AVPacket *packet, const AVCodecParameters *codecpar) {
    if (!packet || !packet->data || packet->size <= 0) return false;

    const int nal_length_size = h264AvccNalLengthSize(codecpar);
    if (nal_length_size <= 0 || nal_length_size > 4) return false;

    const uint8_t *cursor = packet->data;
    int remaining = packet->size;
    while (remaining > nal_length_size) {
        uint32_t nal_size = 0;
        for (int index = 0; index < nal_length_size; ++index) {
            nal_size = (nal_size << 8) | cursor[index];
        }

        cursor += nal_length_size;
        remaining -= nal_length_size;
        if (nal_size == 0 || nal_size > static_cast<uint32_t>(remaining)) break;

        const uint8_t nal_type = cursor[0] & 0x1F;
        if (nal_type == 5) return true;

        cursor += nal_size;
        remaining -= static_cast<int>(nal_size);
    }

    return false;
}

bool packetContainsH264IdrAnnexB(const AVPacket *packet) {
    if (!packet || !packet->data || packet->size <= 0) return false;

    const uint8_t *data = packet->data;
    const size_t size = static_cast<size_t>(packet->size);
    size_t start = 0;
    while (start + 3 < size) {
        size_t nal_start = size;
        size_t prefix_size = 0;
        for (size_t index = start; index + 3 < size; ++index) {
            if (data[index] == 0 && data[index + 1] == 0) {
                if (data[index + 2] == 1) {
                    nal_start = index + 3;
                    prefix_size = 3;
                    break;
                }
                if (index + 3 < size && data[index + 2] == 0 && data[index + 3] == 1) {
                    nal_start = index + 4;
                    prefix_size = 4;
                    break;
                }
            }
        }
        if (nal_start == size || nal_start >= size) break;

        size_t nal_end = size;
        for (size_t index = nal_start; index + 3 < size; ++index) {
            if (data[index] == 0 && data[index + 1] == 0 &&
                (data[index + 2] == 1 ||
                 (index + 3 < size && data[index + 2] == 0 && data[index + 3] == 1))) {
                nal_end = index;
                break;
            }
        }

        if (nal_end > nal_start && (data[nal_start] & 0x1F) == 5) {
            return true;
        }

        start = nal_end > nal_start ? nal_end : (nal_start + prefix_size);
    }

    return false;
}

bool extractH264ParameterSetsFromPacket(
    const AVPacket *packet,
    int nal_length_size,
    std::vector<uint8_t> *sps,
    std::vector<uint8_t> *pps
) {
    if (!packet || !packet->data || packet->size <= 0) return false;
    if (nal_length_size <= 0 || nal_length_size > 4) return false;

    if (sps) sps->clear();
    if (pps) pps->clear();

    const uint8_t *cursor = packet->data;
    int remaining = packet->size;
    while (remaining > nal_length_size) {
        uint32_t nal_size = 0;
        for (int index = 0; index < nal_length_size; ++index) {
            nal_size = (nal_size << 8) | cursor[index];
        }

        cursor += nal_length_size;
        remaining -= nal_length_size;
        if (nal_size == 0 || nal_size > static_cast<uint32_t>(remaining)) break;

        const uint8_t *nal = cursor;
        const uint8_t nal_type = nal[0] & 0x1F;
        if (nal_type == 7 && sps && sps->empty()) {
            sps->assign(nal, nal + nal_size);
        } else if (nal_type == 8 && pps && pps->empty()) {
            pps->assign(nal, nal + nal_size);
        }
        if (sps && pps && !sps->empty() && !pps->empty()) return true;

        cursor += nal_size;
        remaining -= static_cast<int>(nal_size);
    }

    return sps && pps && !sps->empty() && !pps->empty();
}

bool extractH264ParameterSetsFromAnnexB(
    const uint8_t *data,
    size_t size,
    std::vector<uint8_t> *sps,
    std::vector<uint8_t> *pps
) {
    if (!data || size == 0) return false;
    if (sps) sps->clear();
    if (pps) pps->clear();

    size_t start = 0;
    while (start + 3 < size) {
        size_t nal_start = size;
        size_t prefix_size = 0;
        for (size_t index = start; index + 3 < size; ++index) {
            if (data[index] == 0 && data[index + 1] == 0) {
                if (data[index + 2] == 1) {
                    nal_start = index + 3;
                    prefix_size = 3;
                    break;
                }
                if (index + 3 < size && data[index + 2] == 0 && data[index + 3] == 1) {
                    nal_start = index + 4;
                    prefix_size = 4;
                    break;
                }
            }
        }
        if (nal_start == size || nal_start >= size) break;

        size_t nal_end = size;
        for (size_t index = nal_start; index + 3 < size; ++index) {
            if (data[index] == 0 && data[index + 1] == 0 &&
                (data[index + 2] == 1 ||
                 (index + 3 < size && data[index + 2] == 0 && data[index + 3] == 1))) {
                nal_end = index;
                break;
            }
        }

        if (nal_end > nal_start) {
            const uint8_t nal_type = data[nal_start] & 0x1F;
            if (nal_type == 7 && sps && sps->empty()) {
                sps->assign(data + nal_start, data + nal_end);
            } else if (nal_type == 8 && pps && pps->empty()) {
                pps->assign(data + nal_start, data + nal_end);
            }
            if (sps && pps && !sps->empty() && !pps->empty()) return true;
        }

        start = nal_end > nal_start ? nal_end : (nal_start + prefix_size);
    }

    return sps && pps && !sps->empty() && !pps->empty();
}

bool extradataLooksLikeAvcc(const uint8_t *extradata, size_t extradata_size) {
    return extradata && extradata_size >= 5 && extradata[0] == 1;
}

bool extradataLooksLikeAnnexB(const uint8_t *extradata, size_t extradata_size) {
    if (!extradata || extradata_size < 4) return false;
    return (extradata[0] == 0 && extradata[1] == 0 && extradata[2] == 1) ||
        (extradata_size >= 5 &&
         extradata[0] == 0 &&
         extradata[1] == 0 &&
         extradata[2] == 0 &&
         extradata[3] == 1);
}

int appendH264AvccTemplateTail(
    std::vector<uint8_t> *extradata,
    const std::vector<uint8_t> &template_extradata
) {
    if (!extradata || template_extradata.size() < 6 || template_extradata[0] != 1) return 0;

    size_t offset = 6;
    const uint8_t num_sps = template_extradata[5] & 0x1F;
    for (uint8_t index = 0; index < num_sps; ++index) {
        if (offset + 2 > template_extradata.size()) return AVERROR_INVALIDDATA;
        const size_t nal_size = (static_cast<size_t>(template_extradata[offset]) << 8) |
            template_extradata[offset + 1];
        offset += 2;
        if (offset + nal_size > template_extradata.size()) return AVERROR_INVALIDDATA;
        offset += nal_size;
    }

    if (offset + 1 > template_extradata.size()) return AVERROR_INVALIDDATA;
    const uint8_t num_pps = template_extradata[offset++];
    for (uint8_t index = 0; index < num_pps; ++index) {
        if (offset + 2 > template_extradata.size()) return AVERROR_INVALIDDATA;
        const size_t nal_size = (static_cast<size_t>(template_extradata[offset]) << 8) |
            template_extradata[offset + 1];
        offset += 2;
        if (offset + nal_size > template_extradata.size()) return AVERROR_INVALIDDATA;
        offset += nal_size;
    }

    if (offset < template_extradata.size()) {
        extradata->insert(extradata->end(), template_extradata.begin() + static_cast<long>(offset), template_extradata.end());
    }
    return 0;
}

bool buildH264AvccExtradataFromNalUnits(
    std::vector<uint8_t> *snapshot,
    const std::vector<uint8_t> &sps,
    const std::vector<uint8_t> &pps,
    int nal_length_size,
    const std::vector<uint8_t> &template_extradata
) {
    if (!snapshot || sps.size() < 4 || pps.empty()) return false;
    if (nal_length_size <= 0 || nal_length_size > 4) nal_length_size = 4;

    std::vector<uint8_t> extradata;
    extradata.reserve(11 + sps.size() + pps.size() + template_extradata.size());
    extradata.push_back(1);
    extradata.push_back(sps[1]);
    extradata.push_back(sps[2]);
    extradata.push_back(sps[3]);
    extradata.push_back(static_cast<uint8_t>(0xFC | ((nal_length_size - 1) & 0x03)));
    extradata.push_back(0xE1);
    extradata.push_back(static_cast<uint8_t>((sps.size() >> 8) & 0xFF));
    extradata.push_back(static_cast<uint8_t>(sps.size() & 0xFF));
    extradata.insert(extradata.end(), sps.begin(), sps.end());
    extradata.push_back(1);
    extradata.push_back(static_cast<uint8_t>((pps.size() >> 8) & 0xFF));
    extradata.push_back(static_cast<uint8_t>(pps.size() & 0xFF));
    extradata.insert(extradata.end(), pps.begin(), pps.end());

    if (template_extradata.size() >= 4 &&
        template_extradata[0] == 1 &&
        template_extradata[1] == sps[1] &&
        template_extradata[2] == sps[2] &&
        template_extradata[3] == sps[3]) {
        if (appendH264AvccTemplateTail(&extradata, template_extradata) < 0) {
            return false;
        }
    }

    *snapshot = std::move(extradata);
    return true;
}

bool buildH264AvccExtradataFromPacket(
    std::vector<uint8_t> *snapshot,
    const AVPacket *packet,
    const AVCodecParameters *codecpar,
    const std::vector<uint8_t> &template_extradata
) {
    if (!snapshot || !packet || !packet->data || packet->size <= 0) return false;

    const int nal_length_size = h264AvccNalLengthSize(codecpar);
    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;
    if (extractH264ParameterSetsFromPacket(packet, nal_length_size, &sps, &pps) && sps.size() >= 4) {
        return buildH264AvccExtradataFromNalUnits(snapshot, sps, pps, nal_length_size, template_extradata);
    }

    if (!extractH264ParameterSetsFromAnnexB(
            packet->data,
            static_cast<size_t>(packet->size),
            &sps,
            &pps
        ) ||
        sps.size() < 4) {
        return false;
    }

    return buildH264AvccExtradataFromNalUnits(snapshot, sps, pps, nal_length_size, template_extradata);
}

bool normalizeH264ExtradataForMp4(
    std::vector<uint8_t> *snapshot,
    const uint8_t *extradata,
    size_t extradata_size,
    const AVCodecParameters *codecpar,
    const std::vector<uint8_t> &template_extradata
) {
    if (!snapshot || !extradata || extradata_size == 0) return false;
    if (extradataLooksLikeAvcc(extradata, extradata_size)) {
        snapshot->assign(extradata, extradata + extradata_size);
        return true;
    }
    if (!extradataLooksLikeAnnexB(extradata, extradata_size)) return false;

    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;
    if (!extractH264ParameterSetsFromAnnexB(extradata, extradata_size, &sps, &pps)) return false;

    int nal_length_size = 4;
    if (codecpar && codecpar->codec_id == AV_CODEC_ID_H264 && codecpar->extradata_size > 0) {
        nal_length_size = h264AvccNalLengthSize(codecpar);
    } else if (template_extradata.size() >= 5 && template_extradata[0] == 1) {
        nal_length_size = (template_extradata[4] & 0x03) + 1;
    }
    return buildH264AvccExtradataFromNalUnits(snapshot, sps, pps, nal_length_size, template_extradata);
}

bool packetIsVideoResumeSyncPacket(const AVPacket *packet, const AVStream *stream, const AVCodecContext *encoder_context) {
    if (!packet) return false;

    const enum AVCodecID codec_id = encoder_context
        ? encoder_context->codec_id
        : (stream && stream->codecpar ? stream->codecpar->codec_id : AV_CODEC_ID_NONE);
    if (codec_id != AV_CODEC_ID_H264) return (packet->flags & AV_PKT_FLAG_KEY) != 0;

    if (stream && stream->codecpar && packetContainsH264Idr(packet, stream->codecpar)) {
        return true;
    }
    if (packetContainsH264IdrAnnexB(packet)) {
        return true;
    }
    return (packet->flags & AV_PKT_FLAG_KEY) != 0;
}

void assignExtradataSnapshot(
    std::vector<uint8_t> *snapshot,
    const uint8_t *extradata,
    int extradata_size
) {
    if (!snapshot) return;

    snapshot->clear();
    if (!extradata || extradata_size <= 0) return;
    snapshot->assign(extradata, extradata + extradata_size);
}

int replaceCodecparExtradata(AVCodecParameters *codecpar, const std::vector<uint8_t> &extradata) {
    if (!codecpar) return AVERROR(EINVAL);

    av_freep(&codecpar->extradata);
    codecpar->extradata_size = 0;
    if (extradata.empty()) return 0;

    const size_t allocation_size = extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE;
    uint8_t *buffer = static_cast<uint8_t *>(av_mallocz(allocation_size));
    if (!buffer) return AVERROR(ENOMEM);

    memcpy(buffer, extradata.data(), extradata.size());
    codecpar->extradata = buffer;
    codecpar->extradata_size = static_cast<int>(extradata.size());
    return 0;
}

std::string describeExtradata(const std::vector<uint8_t> &extradata);

int syncH264StreamExtradataFromSnapshot(
    AVStream *stream,
    const std::vector<uint8_t> &extradata,
    const char *reason
) {
    if (!stream || !stream->codecpar || stream->codecpar->codec_id != AV_CODEC_ID_H264 || extradata.empty()) {
        return 0;
    }

    const bool extradata_matches = stream->codecpar->extradata &&
        stream->codecpar->extradata_size == static_cast<int>(extradata.size()) &&
        memcmp(stream->codecpar->extradata, extradata.data(), extradata.size()) == 0;
    if (extradata_matches) return 0;

    const int ret = replaceCodecparExtradata(stream->codecpar, extradata);
    if (ret < 0) return ret;

    if (reason && reason[0]) {
        AWESOME_FF_LOGI(
            "Synced H.264 extradata to output stream (%s): %s",
            reason,
            describeExtradata(extradata).c_str()
        );
    } else {
        AWESOME_FF_LOGI(
            "Synced H.264 extradata to output stream: %s",
            describeExtradata(extradata).c_str()
        );
    }
    return 0;
}

std::string describeExtradata(const std::vector<uint8_t> &extradata) {
    if (extradata.empty()) return "size=0";

    char buffer[128];
    const size_t preview = extradata.size() < 8 ? extradata.size() : 8;
    int offset = snprintf(buffer, sizeof(buffer), "size=%zu head=", extradata.size());
    for (size_t index = 0; index < preview && offset > 0 && offset < static_cast<int>(sizeof(buffer)); ++index) {
        offset += snprintf(
            buffer + offset,
            sizeof(buffer) - static_cast<size_t>(offset),
            "%02X",
            extradata[index]
        );
    }
    return std::string(buffer);
}

void refreshVideoExtradataSnapshotFromRuntimeState(
    std::vector<uint8_t> *snapshot,
    const AVCodecContext *encoder_context,
    const AVPacket *packet,
    const AVCodecParameters *codecpar,
    const std::vector<uint8_t> &template_extradata,
    VideoExtradataSource *source
) {
    if (!snapshot) return;
    if (source) *source = kVideoExtradataSourceNone;

    size_t side_size = 0;
    uint8_t *side = packet ? av_packet_get_side_data(
        const_cast<AVPacket *>(packet),
        AV_PKT_DATA_NEW_EXTRADATA,
        &side_size
    ) : nullptr;
    if (side && side_size > 0) {
        if (codecpar && codecpar->codec_id == AV_CODEC_ID_H264 &&
            normalizeH264ExtradataForMp4(snapshot, side, side_size, codecpar, template_extradata)) {
            if (source) *source = kVideoExtradataSourcePacketSideData;
            return;
        }
        assignExtradataSnapshot(snapshot, side, static_cast<int>(side_size));
        if (source) *source = kVideoExtradataSourcePacketSideData;
        return;
    }

    if (encoder_context && encoder_context->extradata && encoder_context->extradata_size > 0) {
        if (encoder_context->codec_id == AV_CODEC_ID_H264 &&
            normalizeH264ExtradataForMp4(
                snapshot,
                encoder_context->extradata,
                static_cast<size_t>(encoder_context->extradata_size),
                codecpar,
                template_extradata
            )) {
            if (source) *source = kVideoExtradataSourceEncoderContext;
            return;
        }
        assignExtradataSnapshot(snapshot, encoder_context->extradata, encoder_context->extradata_size);
        if (source) *source = kVideoExtradataSourceEncoderContext;
        return;
    }

    const enum AVCodecID codec_id = encoder_context
        ? encoder_context->codec_id
        : (codecpar ? codecpar->codec_id : AV_CODEC_ID_NONE);
    if (codec_id == AV_CODEC_ID_H264 &&
        buildH264AvccExtradataFromPacket(snapshot, packet, codecpar, template_extradata)) {
        if (source) *source = kVideoExtradataSourcePacketParameterSets;
        return;
    }
}

int appendPacketExtradataSideData(AVPacket *packet, const std::vector<uint8_t> &extradata) {
    if (!packet || extradata.empty()) return 0;

    uint8_t *side_data = av_packet_new_side_data(
        packet,
        AV_PKT_DATA_NEW_EXTRADATA,
        static_cast<size_t>(extradata.size())
    );
    if (!side_data) return AVERROR(ENOMEM);

    memcpy(side_data, extradata.data(), extradata.size());
    return 0;
}

bool findAnnexBStartCode(
    const uint8_t *data,
    size_t size,
    size_t offset,
    size_t *start_offset,
    size_t *prefix_size
) {
    if (!data || size < 4) return false;

    for (size_t index = offset; index + 3 < size; ++index) {
        if (data[index] != 0 || data[index + 1] != 0) {
            continue;
        }
        if (data[index + 2] == 1) {
            if (start_offset) *start_offset = index;
            if (prefix_size) *prefix_size = 3;
            return true;
        }
        if (index + 3 < size && data[index + 2] == 0 && data[index + 3] == 1) {
            if (start_offset) *start_offset = index;
            if (prefix_size) *prefix_size = 4;
            return true;
        }
    }

    return false;
}

const char *describeH264NalType(uint8_t nal_type) {
    switch (nal_type) {
        case 1: return "slice";
        case 5: return "idr";
        case 6: return "sei";
        case 7: return "sps";
        case 8: return "pps";
        case 9: return "aud";
        default: return "other";
    }
}

bool isPlausibleElementaryH264NalType(uint8_t nal_type) {
    return nal_type > 0 && nal_type < 24;
}

enum H264PacketPayloadLayout {
    kH264PacketPayloadLayoutUnknown = 0,
    kH264PacketPayloadLayoutAnnexB,
    kH264PacketPayloadLayoutAvcc,
};

std::string hexPreview(const uint8_t *data, size_t size, size_t max_bytes = 16) {
    if (!data || size == 0) return "empty";

    std::string preview;
    const size_t limit = size < max_bytes ? size : max_bytes;
    preview.reserve(limit * 2);
    char buffer[4];
    for (size_t index = 0; index < limit; ++index) {
        snprintf(buffer, sizeof(buffer), "%02X", data[index]);
        preview += buffer;
    }
    return preview;
}

void appendH264NalSummaryEntry(
    std::string *summary,
    int index,
    uint8_t nal_type,
    size_t nal_size
) {
    if (!summary) return;
    if (!summary->empty()) *summary += "|";
    *summary += std::to_string(index);
    *summary += ":";
    *summary += describeH264NalType(nal_type);
    *summary += "(";
    *summary += std::to_string(nal_type);
    *summary += ",";
    *summary += std::to_string(nal_size);
    *summary += ")";
}

bool summarizeH264AnnexBPacket(const AVPacket *packet, std::string *summary, bool require_plausible_nal_types = false) {
    if (!packet || !packet->data || packet->size <= 0 || !summary) return false;

    summary->clear();
    const uint8_t *data = packet->data;
    const size_t size = static_cast<size_t>(packet->size);
    size_t start_offset = 0;
    size_t prefix_size = 0;
    size_t search_offset = 0;
    int nal_index = 0;

    while (findAnnexBStartCode(data, size, search_offset, &start_offset, &prefix_size) && nal_index < 6) {
        const size_t nal_start = start_offset + prefix_size;
        size_t next_start_offset = 0;
        size_t next_prefix_size = 0;
        const bool has_next = findAnnexBStartCode(
            data,
            size,
            nal_start,
            &next_start_offset,
            &next_prefix_size
        );
        (void)next_prefix_size;

        const size_t nal_end = has_next ? next_start_offset : size;
        if (nal_end <= nal_start) break;

        const uint8_t nal_type = data[nal_start] & 0x1F;
        if (require_plausible_nal_types && !isPlausibleElementaryH264NalType(nal_type)) {
            summary->clear();
            return false;
        }
        appendH264NalSummaryEntry(summary, nal_index, nal_type, nal_end - nal_start);
        ++nal_index;
        search_offset = nal_end;
        if (!has_next) break;
    }

    return nal_index > 0;
}

bool summarizeH264AvccPacket(
    const AVPacket *packet,
    const AVCodecParameters *codecpar,
    std::string *summary,
    bool require_plausible_nal_types = false
) {
    if (!packet || !packet->data || packet->size <= 0 || !summary) return false;

    summary->clear();
    int nal_length_size = h264AvccNalLengthSize(codecpar);
    if (nal_length_size <= 0 || nal_length_size > 4) return false;

    const uint8_t *cursor = packet->data;
    int remaining = packet->size;
    int nal_index = 0;
    while (remaining > nal_length_size && nal_index < 6) {
        uint32_t nal_size = 0;
        for (int index = 0; index < nal_length_size; ++index) {
            nal_size = (nal_size << 8) | cursor[index];
        }

        cursor += nal_length_size;
        remaining -= nal_length_size;
        if (nal_size == 0 || nal_size > static_cast<uint32_t>(remaining)) {
            return false;
        }

        const uint8_t nal_type = cursor[0] & 0x1F;
        if (require_plausible_nal_types && !isPlausibleElementaryH264NalType(nal_type)) {
            summary->clear();
            return false;
        }
        appendH264NalSummaryEntry(summary, nal_index, nal_type, nal_size);
        cursor += nal_size;
        remaining -= static_cast<int>(nal_size);
        ++nal_index;
    }

    return nal_index > 0;
}

H264PacketPayloadLayout detectH264PacketPayloadLayout(const AVPacket *packet, const AVCodecParameters *codecpar) {
    std::string summary;
    const bool avcc_ok = summarizeH264AvccPacket(packet, codecpar, &summary, true);
    const bool annexb_ok = summarizeH264AnnexBPacket(packet, &summary, true);
    const bool stream_prefers_avcc = codecpar &&
        extradataLooksLikeAvcc(
            codecpar->extradata,
            static_cast<size_t>(codecpar->extradata_size > 0 ? codecpar->extradata_size : 0)
        );

    if (avcc_ok && !annexb_ok) return kH264PacketPayloadLayoutAvcc;
    if (annexb_ok && !avcc_ok) return kH264PacketPayloadLayoutAnnexB;
    if (avcc_ok && annexb_ok) {
        return stream_prefers_avcc ? kH264PacketPayloadLayoutAvcc : kH264PacketPayloadLayoutAnnexB;
    }
    return kH264PacketPayloadLayoutUnknown;
}

std::string describeH264PacketPayload(const AVPacket *packet, const AVCodecParameters *codecpar) {
    if (!packet || !packet->data || packet->size <= 0) return "empty";

    std::string nal_summary;
    switch (detectH264PacketPayloadLayout(packet, codecpar)) {
        case kH264PacketPayloadLayoutAnnexB:
            summarizeH264AnnexBPacket(packet, &nal_summary, false);
            return "annexb head=" + hexPreview(packet->data, static_cast<size_t>(packet->size)) + " nals=" + nal_summary;
        case kH264PacketPayloadLayoutAvcc:
            summarizeH264AvccPacket(packet, codecpar, &nal_summary, false);
            return "avcc(len=" + std::to_string(h264AvccNalLengthSize(codecpar)) +
                ") head=" + hexPreview(packet->data, static_cast<size_t>(packet->size)) +
                " nals=" + nal_summary;
        case kH264PacketPayloadLayoutUnknown:
        default:
            break;
    }

    if (summarizeH264AnnexBPacket(packet, &nal_summary, false)) {
        return "annexb head=" + hexPreview(packet->data, static_cast<size_t>(packet->size)) + " nals=" + nal_summary;
    }
    if (summarizeH264AvccPacket(packet, codecpar, &nal_summary, false)) {
        return "avcc(len=" + std::to_string(h264AvccNalLengthSize(codecpar)) +
            ") head=" + hexPreview(packet->data, static_cast<size_t>(packet->size)) +
            " nals=" + nal_summary;
    }
    return "unknown head=" + hexPreview(packet->data, static_cast<size_t>(packet->size));
}

void logVideoPacketDebugSummary(
    int packet_debug_index,
    const std::string &before_conversion,
    const AVPacket *packet,
    const AVCodecParameters *codecpar
) {
    if (!packet || !codecpar) return;

    size_t side_extradata_size = 0;
    const uint8_t *side_extradata = av_packet_get_side_data(
        const_cast<AVPacket *>(packet),
        AV_PKT_DATA_NEW_EXTRADATA,
        &side_extradata_size
    );
    const uint8_t *stream_extradata = codecpar->extradata;
    const size_t stream_extradata_size = codecpar->extradata_size > 0
        ? static_cast<size_t>(codecpar->extradata_size)
        : 0;

    AWESOME_FF_LOGI(
        "Video packet debug[%d]: before=%s final=%s side_extradata=%zu(%s) stream_extradata=%zu(%s)",
        packet_debug_index,
        before_conversion.c_str(),
        describeH264PacketPayload(packet, codecpar).c_str(),
        side_extradata_size,
        hexPreview(side_extradata, side_extradata_size).c_str(),
        stream_extradata_size,
        hexPreview(stream_extradata, stream_extradata_size).c_str()
    );
}

int convertH264PacketFromAnnexBToAvcc(AVPacket *packet, const AVCodecParameters *codecpar) {
    if (!packet || !packet->data || packet->size <= 0) return 0;
    if (!extradataLooksLikeAnnexB(packet->data, static_cast<size_t>(packet->size))) return 0;

    int nal_length_size = h264AvccNalLengthSize(codecpar);
    if (nal_length_size <= 0 || nal_length_size > 4) {
        nal_length_size = 4;
    }

    const uint8_t *data = packet->data;
    const size_t size = static_cast<size_t>(packet->size);
    std::vector<uint8_t> avcc_data;
    avcc_data.reserve(size + 16);

    size_t start_offset = 0;
    size_t prefix_size = 0;
    bool found_nal = false;
    size_t search_offset = 0;
    while (findAnnexBStartCode(data, size, search_offset, &start_offset, &prefix_size)) {
        const size_t nal_start = start_offset + prefix_size;
        size_t next_start_offset = 0;
        size_t next_prefix_size = 0;
        const bool has_next = findAnnexBStartCode(
            data,
            size,
            nal_start,
            &next_start_offset,
            &next_prefix_size
        );
        (void)next_prefix_size;

        const size_t nal_end = has_next ? next_start_offset : size;
        if (nal_end > nal_start) {
            const size_t nal_size = nal_end - nal_start;
            if (nal_size > static_cast<size_t>(INT_MAX)) return AVERROR_INVALIDDATA;

            for (int shift = nal_length_size - 1; shift >= 0; --shift) {
                avcc_data.push_back(static_cast<uint8_t>((nal_size >> (shift * 8)) & 0xFF));
            }
            avcc_data.insert(
                avcc_data.end(),
                data + nal_start,
                data + nal_end
            );
            found_nal = true;
        }

        search_offset = nal_end;
        if (!has_next) {
            break;
        }
    }

    if (!found_nal || avcc_data.empty()) return 0;

    AVPacket converted_packet = {0};
    int ret = av_new_packet(&converted_packet, static_cast<int>(avcc_data.size()));
    if (ret < 0) return ret;

    memcpy(converted_packet.data, avcc_data.data(), avcc_data.size());

    ret = av_packet_copy_props(&converted_packet, packet);
    if (ret < 0) {
        av_packet_unref(&converted_packet);
        return ret;
    }

    converted_packet.pts = packet->pts;
    converted_packet.dts = packet->dts;
    converted_packet.duration = packet->duration;
    converted_packet.flags = packet->flags;
    converted_packet.stream_index = packet->stream_index;
    converted_packet.pos = packet->pos;

    av_packet_unref(packet);
    av_packet_move_ref(packet, &converted_packet);
    return 1;
}

int buildH264AvccParameterSetPrefix(
    std::vector<uint8_t> *prefix,
    const std::vector<uint8_t> &extradata
) {
    if (!prefix) return AVERROR(EINVAL);
    prefix->clear();
    if (extradata.size() < 6 || extradata[0] != 1) return 0;

    const int nal_length_size = (extradata[4] & 0x03) + 1;
    if (nal_length_size <= 0 || nal_length_size > 4) return AVERROR_INVALIDDATA;

    size_t offset = 6;
    const uint8_t num_sps = extradata[5] & 0x1F;
    for (uint8_t index = 0; index < num_sps; ++index) {
        if (offset + 2 > extradata.size()) return AVERROR_INVALIDDATA;
        const size_t nal_size = (static_cast<size_t>(extradata[offset]) << 8) | extradata[offset + 1];
        offset += 2;
        if (offset + nal_size > extradata.size()) return AVERROR_INVALIDDATA;
        for (int shift = nal_length_size - 1; shift >= 0; --shift) {
            prefix->push_back(static_cast<uint8_t>((nal_size >> (shift * 8)) & 0xFF));
        }
        prefix->insert(prefix->end(), extradata.begin() + static_cast<long>(offset), extradata.begin() + static_cast<long>(offset + nal_size));
        offset += nal_size;
    }

    if (offset + 1 > extradata.size()) return AVERROR_INVALIDDATA;
    const uint8_t num_pps = extradata[offset++];
    for (uint8_t index = 0; index < num_pps; ++index) {
        if (offset + 2 > extradata.size()) return AVERROR_INVALIDDATA;
        const size_t nal_size = (static_cast<size_t>(extradata[offset]) << 8) | extradata[offset + 1];
        offset += 2;
        if (offset + nal_size > extradata.size()) return AVERROR_INVALIDDATA;
        for (int shift = nal_length_size - 1; shift >= 0; --shift) {
            prefix->push_back(static_cast<uint8_t>((nal_size >> (shift * 8)) & 0xFF));
        }
        prefix->insert(prefix->end(), extradata.begin() + static_cast<long>(offset), extradata.begin() + static_cast<long>(offset + nal_size));
        offset += nal_size;
    }

    return 0;
}

int buildH264AnnexBParameterSetPrefix(
    std::vector<uint8_t> *prefix,
    const std::vector<uint8_t> &extradata
) {
    if (!prefix) return AVERROR(EINVAL);
    prefix->clear();
    if (extradata.size() < 6 || extradata[0] != 1) return 0;

    size_t offset = 6;
    const uint8_t num_sps = extradata[5] & 0x1F;
    for (uint8_t index = 0; index < num_sps; ++index) {
        if (offset + 2 > extradata.size()) return AVERROR_INVALIDDATA;
        const size_t nal_size = (static_cast<size_t>(extradata[offset]) << 8) | extradata[offset + 1];
        offset += 2;
        if (offset + nal_size > extradata.size()) return AVERROR_INVALIDDATA;
        prefix->push_back(0);
        prefix->push_back(0);
        prefix->push_back(0);
        prefix->push_back(1);
        prefix->insert(prefix->end(), extradata.begin() + static_cast<long>(offset), extradata.begin() + static_cast<long>(offset + nal_size));
        offset += nal_size;
    }

    if (offset + 1 > extradata.size()) return AVERROR_INVALIDDATA;
    const uint8_t num_pps = extradata[offset++];
    for (uint8_t index = 0; index < num_pps; ++index) {
        if (offset + 2 > extradata.size()) return AVERROR_INVALIDDATA;
        const size_t nal_size = (static_cast<size_t>(extradata[offset]) << 8) | extradata[offset + 1];
        offset += 2;
        if (offset + nal_size > extradata.size()) return AVERROR_INVALIDDATA;
        prefix->push_back(0);
        prefix->push_back(0);
        prefix->push_back(0);
        prefix->push_back(1);
        prefix->insert(prefix->end(), extradata.begin() + static_cast<long>(offset), extradata.begin() + static_cast<long>(offset + nal_size));
        offset += nal_size;
    }

    return 0;
}

int prependVideoExtradataParameterSetsToPacket(
    AVPacket *packet,
    const std::vector<uint8_t> &extradata,
    const AVCodecParameters *codecpar
) {
    if (!packet || extradata.empty()) return 0;
    if (!codecpar || codecpar->codec_id != AV_CODEC_ID_H264) return 0;

    std::vector<uint8_t> packet_sps;
    std::vector<uint8_t> packet_pps;
    if (extractH264ParameterSetsFromPacket(packet, h264AvccNalLengthSize(codecpar), &packet_sps, &packet_pps) ||
        extractH264ParameterSetsFromAnnexB(packet->data, static_cast<size_t>(packet->size), &packet_sps, &packet_pps)) {
        return 0;
    }

    std::vector<uint8_t> prefix;
    const bool packet_uses_annexb = extradataLooksLikeAnnexB(packet->data, static_cast<size_t>(packet->size));
    int ret = packet_uses_annexb
        ? buildH264AnnexBParameterSetPrefix(&prefix, extradata)
        : buildH264AvccParameterSetPrefix(&prefix, extradata);
    if (ret < 0 || prefix.empty()) return ret;

    AVPacket prefixed_packet = {0};
    ret = av_new_packet(&prefixed_packet, static_cast<int>(prefix.size()) + packet->size);
    if (ret < 0) return ret;

    memcpy(prefixed_packet.data, prefix.data(), prefix.size());
    memcpy(prefixed_packet.data + prefix.size(), packet->data, static_cast<size_t>(packet->size));

    ret = av_packet_copy_props(&prefixed_packet, packet);
    if (ret < 0) {
        av_packet_unref(&prefixed_packet);
        return ret;
    }

    prefixed_packet.pts = packet->pts;
    prefixed_packet.dts = packet->dts;
    prefixed_packet.duration = packet->duration;
    prefixed_packet.flags = packet->flags;
    prefixed_packet.stream_index = packet->stream_index;
    prefixed_packet.pos = packet->pos;

    av_packet_unref(packet);
    av_packet_move_ref(packet, &prefixed_packet);
    return 1;
}

int initializeBitstreamFilterForStream(
    AVBSFContext **context,
    AVStream *stream,
    const char *name
) {
    if (!context || !stream || !name || !name[0]) return AVERROR(EINVAL);

    if (*context) {
        av_bsf_free(context);
    }

    const AVBitStreamFilter *filter = av_bsf_get_by_name(name);
    if (!filter) return AVERROR_FILTER_NOT_FOUND;

    int ret = av_bsf_alloc(filter, context);
    if (ret < 0) return ret;

    ret = avcodec_parameters_copy((*context)->par_in, stream->codecpar);
    if (ret < 0) {
        av_bsf_free(context);
        return ret;
    }

    (*context)->time_base_in = stream->time_base;
    ret = av_bsf_init(*context);
    if (ret < 0) {
        av_bsf_free(context);
        return ret;
    }

    return 0;
}

} // namespace

FFmpegMp4TranscoderBase::FFmpegMp4TranscoderBase(
    const char *input_path,
    const char *output_path,
    const FFmpegMp4TranscodeConfig *config,
    void (*progress_cb)(int percentage, void *user_data),
    void *user_data,
    volatile int *cancel_flag,
    volatile int *pause_flag
) : input_path_(input_path ? input_path : ""),
    output_path_(output_path ? output_path : ""),
    video_preset_(defaultPreset(config)),
    video_profile_(defaultProfile(config)),
    video_level_(defaultLevel(config)),
    video_crf_(defaultCrf(config)),
    audio_bitrate_(defaultAudioBitrate(config)),
    faststart_(defaultFaststart(config)),
    frame_rate_(defaultFrameRate(config)),
    scale_width_(defaultScaleWidth(config)),
    scale_height_(defaultScaleHeight(config)),
    progress_cb_(progress_cb),
    user_data_(user_data),
    cancel_flag_(cancel_flag),
    pause_flag_(pause_flag),
    input_format_(nullptr),
    output_format_(nullptr),
    video_dec_ctx_(nullptr),
    video_enc_ctx_(nullptr),
    audio_dec_ctx_(nullptr),
    audio_enc_ctx_(nullptr),
    sws_ctx_(nullptr),
    swr_ctx_(nullptr),
    audio_fifo_(nullptr),
    video_dec_frame_(nullptr),
    audio_dec_frame_(nullptr),
    enc_packet_(nullptr),
    audio_copy_bsf_ctx_(nullptr),
    video_input_index_(-1),
    video_output_index_(-1),
    audio_input_index_(-1),
    audio_output_index_(-1),
    audio_next_pts_(AV_NOPTS_VALUE),
    timeline_start_us_(AV_NOPTS_VALUE),
    video_encoder_input_origin_pts_(AV_NOPTS_VALUE),
    video_encoder_output_offset_pts_(AV_NOPTS_VALUE),
    video_last_written_pts_(AV_NOPTS_VALUE),
    video_last_written_dts_(AV_NOPTS_VALUE),
    video_last_write_packet_count_(0),
    video_debug_packets_logged_(0),
    audio_copy_mode_(false),
    video_force_keyframe_on_next_frame_(false),
    video_packets_written_(false),
    output_header_written_(false),
    video_muxer_reopen_required_(false),
    video_pending_extradata_refresh_(false),
    video_attach_extradata_to_next_packet_(false),
    video_drop_until_sync_packet_(false),
    video_require_fresh_extradata_(false),
    video_current_extradata_observed_(false),
    pause_observed_(false),
    video_rebuild_after_pause_pending_(false),
    video_extradata_snapshot_(),
    progress_() {
    FFmpegCommonUtils::installPlatformLogBridge();
}

FFmpegMp4TranscoderBase::FFmpegMp4TranscoderBase(
    const char *input_path,
    const char *output_path,
    void (*progress_cb)(int percentage, void *user_data),
    void *user_data,
    volatile int *cancel_flag,
    volatile int *pause_flag
) : FFmpegMp4TranscoderBase(
        input_path,
        output_path,
        nullptr,
        progress_cb,
        user_data,
        cancel_flag,
        pause_flag
    ) {
}

void FFmpegMp4TranscoderBase::cleanupSharedResources() {
    if (enc_packet_) av_packet_free(&enc_packet_);
    if (audio_copy_bsf_ctx_) av_bsf_free(&audio_copy_bsf_ctx_);
    if (audio_dec_frame_) av_frame_free(&audio_dec_frame_);
    if (video_dec_frame_) av_frame_free(&video_dec_frame_);
    if (audio_fifo_) av_audio_fifo_free(audio_fifo_);
    if (swr_ctx_) swr_free(&swr_ctx_);
    if (sws_ctx_) sws_freeContext(sws_ctx_);
    if (audio_enc_ctx_) avcodec_free_context(&audio_enc_ctx_);
    if (audio_dec_ctx_) avcodec_free_context(&audio_dec_ctx_);
    if (video_enc_ctx_) avcodec_free_context(&video_enc_ctx_);
    if (video_dec_ctx_) avcodec_free_context(&video_dec_ctx_);
    if (input_format_) avformat_close_input(&input_format_);
    if (output_format_) {
        if (output_format_->pb) avio_closep(&output_format_->pb);
        avformat_free_context(output_format_);
    }

    output_format_ = nullptr;
    input_format_ = nullptr;
    video_dec_ctx_ = nullptr;
    video_enc_ctx_ = nullptr;
    audio_dec_ctx_ = nullptr;
    audio_enc_ctx_ = nullptr;
    sws_ctx_ = nullptr;
    swr_ctx_ = nullptr;
    audio_fifo_ = nullptr;
    video_dec_frame_ = nullptr;
    audio_dec_frame_ = nullptr;
    enc_packet_ = nullptr;
    audio_copy_bsf_ctx_ = nullptr;
    video_input_index_ = -1;
    video_output_index_ = -1;
    audio_input_index_ = -1;
    audio_output_index_ = -1;
    audio_next_pts_ = AV_NOPTS_VALUE;
    timeline_start_us_ = AV_NOPTS_VALUE;
    video_encoder_input_origin_pts_ = AV_NOPTS_VALUE;
    video_encoder_output_offset_pts_ = AV_NOPTS_VALUE;
    video_last_written_pts_ = AV_NOPTS_VALUE;
    video_last_written_dts_ = AV_NOPTS_VALUE;
    video_last_write_packet_count_ = 0;
    video_debug_packets_logged_ = 0;
    audio_copy_mode_ = false;
    video_force_keyframe_on_next_frame_ = false;
    video_packets_written_ = false;
    output_header_written_ = false;
    video_muxer_reopen_required_ = false;
    video_pending_extradata_refresh_ = false;
    video_attach_extradata_to_next_packet_ = false;
    video_drop_until_sync_packet_ = false;
    video_require_fresh_extradata_ = false;
    video_current_extradata_observed_ = false;
    pause_observed_ = false;
    video_rebuild_after_pause_pending_ = false;
    video_extradata_snapshot_.clear();
}

bool FFmpegMp4TranscoderBase::isCancelled() const {
    return cancel_flag_ && *cancel_flag_;
}

bool FFmpegMp4TranscoderBase::isPaused() const {
    return pause_flag_ && *pause_flag_;
}

int FFmpegMp4TranscoderBase::waitIfPaused() {
    while (isPaused()) {
        if (!pause_observed_) {
            pause_observed_ = true;
            AWESOME_FF_LOGI("Transcode paused.");
        }
        if (isCancelled()) return AVERROR_EXIT;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (pause_observed_) {
        pause_observed_ = false;
        AWESOME_FF_LOGI("Transcode resumed.");
        if (video_packets_written_ && shouldRebuildHardwareVideoEncoderAfterPause()) {
            video_rebuild_after_pause_pending_ = true;
            AWESOME_FF_LOGI("Scheduled hardware video encoder rebuild after pause/resume.");
        }
    }
    return 0;
}

bool FFmpegMp4TranscoderBase::hasForcedVideoFrameRate() const {
    return frame_rate_ > 0;
}

AVRational FFmpegMp4TranscoderBase::resolveVideoFrameRate(
    AVFormatContext *format_context,
    AVStream *input_stream
) const {
    if (hasForcedVideoFrameRate()) {
        return AVRational{frame_rate_, 1};
    }
    return fallbackVideoFrameRate(format_context, input_stream);
}

int FFmpegMp4TranscoderBase::videoGopSize(AVRational frame_rate) const {
    if (frame_rate.num <= 0 || frame_rate.den <= 0) return 25;

    const int gop_size = static_cast<int>(
        av_rescale_rnd(frame_rate.num, 1, frame_rate.den, AV_ROUND_NEAR_INF)
    );
    return gop_size > 0 ? gop_size : 25;
}

int FFmpegMp4TranscoderBase::cacheFrameReference(AVFrame **destination, const AVFrame *source) const {
    if (!destination) return AVERROR(EINVAL);

    if (!*destination) {
        *destination = av_frame_alloc();
        if (!*destination) return AVERROR(ENOMEM);
    }

    av_frame_unref(*destination);
    if (!source) return 0;
    return av_frame_ref(*destination, source);
}

int64_t FFmpegMp4TranscoderBase::normalizePts(
    int64_t pts,
    AVRational source_time_base,
    AVRational destination_time_base,
    int64_t output_offset_pts
) const {
    if (pts == AV_NOPTS_VALUE) return AV_NOPTS_VALUE;

    int64_t normalized = av_rescale_q(pts, source_time_base, destination_time_base);
    if (timeline_start_us_ != AV_NOPTS_VALUE) {
        normalized -= av_rescale_q(timeline_start_us_, AV_TIME_BASE_Q, destination_time_base);
    }
    normalized += output_offset_pts;
    if (normalized < 0) normalized = 0;
    return normalized;
}

int FFmpegMp4TranscoderBase::normalizeInputVideoPacketForDecoder(AVPacket *packet) const {
    if (!packet || !input_format_ || video_input_index_ < 0) return 0;

    AVStream *input_stream = input_format_->streams[video_input_index_];
    if (!input_stream || !input_stream->codecpar) return 0;
    if (input_stream->codecpar->codec_id != AV_CODEC_ID_H264) return 0;
    if (!extradataLooksLikeAvcc(
            input_stream->codecpar->extradata,
            static_cast<size_t>(input_stream->codecpar->extradata_size > 0 ? input_stream->codecpar->extradata_size : 0)
        )) {
        return 0;
    }
    const H264PacketPayloadLayout detected_layout = detectH264PacketPayloadLayout(packet, input_stream->codecpar);
    if (detected_layout != kH264PacketPayloadLayoutAnnexB) {
        return 0;
    }

    const std::string before_layout = describeH264PacketPayload(packet, input_stream->codecpar);
    const int ret = convertH264PacketFromAnnexBToAvcc(packet, input_stream->codecpar);
    if (ret > 0) {
        AWESOME_FF_LOGW(
            "Normalized mixed-format H.264 input packet for decoder: before=%s after=%s",
            before_layout.c_str(),
            describeH264PacketPayload(packet, input_stream->codecpar).c_str()
        );
    }
    return ret < 0 ? ret : 0;
}

int FFmpegMp4TranscoderBase::openInputFileAndInitDecoders() {
    int ret = avformat_open_input(&input_format_, input_path_.c_str(), nullptr, nullptr);
    if (ret < 0) return ret;

    ret = avformat_find_stream_info(input_format_, nullptr);
    if (ret < 0) return ret;

    if (input_format_->start_time != AV_NOPTS_VALUE) {
        timeline_start_us_ = input_format_->start_time;
    }

    video_input_index_ = av_find_best_stream(input_format_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_input_index_ < 0) return video_input_index_;

    audio_input_index_ = av_find_best_stream(
        input_format_,
        AVMEDIA_TYPE_AUDIO,
        -1,
        video_input_index_,
        nullptr,
        0
    );

    if (timeline_start_us_ == AV_NOPTS_VALUE) {
        AVStream *video_stream = input_format_->streams[video_input_index_];
        if (video_stream->start_time != AV_NOPTS_VALUE) {
            timeline_start_us_ = av_rescale_q(video_stream->start_time, video_stream->time_base, AV_TIME_BASE_Q);
        }

        if (audio_input_index_ >= 0) {
            AVStream *audio_stream = input_format_->streams[audio_input_index_];
            if (audio_stream->start_time != AV_NOPTS_VALUE) {
                const int64_t audio_start = av_rescale_q(audio_stream->start_time, audio_stream->time_base, AV_TIME_BASE_Q);
                if (timeline_start_us_ == AV_NOPTS_VALUE || audio_start < timeline_start_us_) {
                    timeline_start_us_ = audio_start;
                }
            }
        }
    }

    ret = initializeVideoDecoder();
    if (ret < 0) return ret;

    if (audio_input_index_ >= 0) {
        ret = initializeAudioDecoder();
        if (ret < 0) return ret;
    }

    return 0;
}

int FFmpegMp4TranscoderBase::initializeVideoDecoder() {
    AVStream *input_stream = input_format_->streams[video_input_index_];
    const AVCodec *decoder = avcodec_find_decoder(input_stream->codecpar->codec_id);
    if (!decoder) return AVERROR_DECODER_NOT_FOUND;

    video_dec_ctx_ = avcodec_alloc_context3(decoder);
    if (!video_dec_ctx_) return AVERROR(ENOMEM);

    int ret = avcodec_parameters_to_context(video_dec_ctx_, input_stream->codecpar);
    if (ret < 0) return ret;

    video_dec_ctx_->pkt_timebase = input_stream->time_base;
    return avcodec_open2(video_dec_ctx_, decoder, nullptr);
}

int FFmpegMp4TranscoderBase::initializeAudioDecoder() {
    AVStream *input_stream = input_format_->streams[audio_input_index_];
    const AVCodec *decoder = avcodec_find_decoder(input_stream->codecpar->codec_id);
    if (!decoder) return AVERROR_DECODER_NOT_FOUND;

    audio_dec_ctx_ = avcodec_alloc_context3(decoder);
    if (!audio_dec_ctx_) return AVERROR(ENOMEM);

    int ret = avcodec_parameters_to_context(audio_dec_ctx_, input_stream->codecpar);
    if (ret < 0) return ret;

    audio_dec_ctx_->pkt_timebase = input_stream->time_base;
    return avcodec_open2(audio_dec_ctx_, decoder, nullptr);
}

int FFmpegMp4TranscoderBase::writeHeader() {
    AVDictionary *muxer_options = nullptr;
    if (faststart_) {
        av_dict_set(&muxer_options, "movflags", "+faststart", 0);
    }

    const int ret = avformat_write_header(output_format_, &muxer_options);
    av_dict_free(&muxer_options);
    if (ret >= 0) output_header_written_ = true;
    return ret;
}

int FFmpegMp4TranscoderBase::initializeAudioCopyStreamWithOptionalBsf(AVStream *output_stream) {
    if (!input_format_ || !output_stream || audio_input_index_ < 0) return AVERROR(EINVAL);

    AVStream *input_stream = input_format_->streams[audio_input_index_];
    int ret = 0;

    if (audio_copy_bsf_ctx_) {
        av_bsf_free(&audio_copy_bsf_ctx_);
    }

    if (input_stream->codecpar->codec_id == AV_CODEC_ID_AAC &&
        input_stream->codecpar->extradata_size == 0) {
        ret = initializeBitstreamFilterForStream(&audio_copy_bsf_ctx_, input_stream, "aac_adtstoasc");
        if (ret < 0) return ret;

        ret = avcodec_parameters_copy(output_stream->codecpar, audio_copy_bsf_ctx_->par_out);
        if (ret < 0) return ret;
        output_stream->time_base = audio_copy_bsf_ctx_->time_base_out;
    } else {
        ret = avcodec_parameters_copy(output_stream->codecpar, input_stream->codecpar);
        if (ret < 0) return ret;
        output_stream->time_base = input_stream->time_base;
    }

    audio_output_index_ = output_stream->index;
    audio_copy_mode_ = true;
    output_stream->codecpar->codec_tag = 0;
    output_stream->disposition = input_stream->disposition;
    av_dict_copy(&output_stream->metadata, input_stream->metadata, 0);
    return 0;
}

int FFmpegMp4TranscoderBase::processAudioCopyPacketWithOptionalBsf(AVPacket *packet) {
    if (!packet || !output_format_ || audio_output_index_ < 0 || audio_input_index_ < 0) {
        return AVERROR(EINVAL);
    }

    int ret = waitIfPaused();
    if (ret < 0) return ret;

    if (!audio_copy_bsf_ctx_) {
        return writeAudioCopyPacketToMuxer(packet, input_format_->streams[audio_input_index_]->time_base);
    }

    ret = av_bsf_send_packet(audio_copy_bsf_ctx_, packet);
    if (ret < 0) return ret;

    while ((ret = av_bsf_receive_packet(audio_copy_bsf_ctx_, packet)) == 0) {
        const int write_ret = writeAudioCopyPacketToMuxer(packet, audio_copy_bsf_ctx_->time_base_out);
        av_packet_unref(packet);
        if (write_ret < 0) return write_ret;
    }

    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return 0;
    return ret;
}

int FFmpegMp4TranscoderBase::flushAudioCopyPacketBsf() {
    if (!audio_copy_bsf_ctx_) return 0;

    AVPacket packet = {0};
    int ret = waitIfPaused();
    if (ret < 0) return ret;

    ret = av_bsf_send_packet(audio_copy_bsf_ctx_, nullptr);
    if (ret < 0 && ret != AVERROR_EOF) return ret;

    while ((ret = av_bsf_receive_packet(audio_copy_bsf_ctx_, &packet)) == 0) {
        const int write_ret = writeAudioCopyPacketToMuxer(&packet, audio_copy_bsf_ctx_->time_base_out);
        av_packet_unref(&packet);
        if (write_ret < 0) return write_ret;
    }

    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return 0;
    return ret;
}

int FFmpegMp4TranscoderBase::writeAudioCopyPacketToMuxer(
    AVPacket *packet,
    AVRational source_time_base
) {
    if (!packet || !output_format_ || audio_output_index_ < 0) return AVERROR(EINVAL);

    const int pause_ret = waitIfPaused();
    if (pause_ret < 0) return pause_ret;

    AVStream *output_stream = output_format_->streams[audio_output_index_];
    packet->pts = normalizePts(packet->pts, source_time_base, output_stream->time_base);
    packet->dts = normalizePts(packet->dts, source_time_base, output_stream->time_base);
    if (packet->pts != AV_NOPTS_VALUE &&
        packet->dts != AV_NOPTS_VALUE &&
        packet->dts > packet->pts) {
        packet->dts = packet->pts;
    }
    if (packet->duration > 0) {
        packet->duration = av_rescale_q(packet->duration, source_time_base, output_stream->time_base);
    }

    packet->stream_index = audio_output_index_;
    packet->pos = -1;
    return av_interleaved_write_frame(output_format_, packet);
}

void FFmpegMp4TranscoderBase::prepareVideoFrameForEncoding(AVFrame *frame, int64_t absolute_pts) {
    if (!frame) return;

    if (absolute_pts == AV_NOPTS_VALUE) {
        absolute_pts = 0;
    }
    if (video_encoder_input_origin_pts_ == AV_NOPTS_VALUE) {
        video_encoder_input_origin_pts_ = absolute_pts;
    }

    frame->pts = absolute_pts - video_encoder_input_origin_pts_;
    if (frame->pts < 0) frame->pts = 0;

    if (video_force_keyframe_on_next_frame_) {
        frame->pict_type = AV_PICTURE_TYPE_I;
        frame->flags |= AV_FRAME_FLAG_KEY;
        video_force_keyframe_on_next_frame_ = false;
    } else {
        frame->pict_type = AV_PICTURE_TYPE_NONE;
        frame->flags &= ~AV_FRAME_FLAG_KEY;
    }
}

void FFmpegMp4TranscoderBase::markVideoEncoderRebuilt() {
    video_encoder_input_origin_pts_ = AV_NOPTS_VALUE;
    video_encoder_output_offset_pts_ = AV_NOPTS_VALUE;
    video_force_keyframe_on_next_frame_ = true;
    video_drop_until_sync_packet_ = true;
    video_require_fresh_extradata_ = false;
    video_current_extradata_observed_ = !video_extradata_snapshot_.empty();
    video_rebuild_after_pause_pending_ = false;
}

bool FFmpegMp4TranscoderBase::shouldRebuildHardwareVideoEncoderAfterPause() const {
    return video_enc_ctx_ &&
        video_enc_ctx_->codec &&
        FFmpegCommonUtils::isVideoToolboxEncoder(video_enc_ctx_->codec);
}

void FFmpegMp4TranscoderBase::logOutputFileProbeSummary() const {
    if (output_path_.empty()) return;

    AVFormatContext *probe_format = nullptr;
    int ret = avformat_open_input(&probe_format, output_path_.c_str(), nullptr, nullptr);
    if (ret < 0) {
        FFmpegCommonUtils::printError("Failed to reopen muxed output for probe", ret);
        return;
    }

    ret = avformat_find_stream_info(probe_format, nullptr);
    if (ret < 0) {
        FFmpegCommonUtils::printError("Failed to read muxed output stream info", ret);
        avformat_close_input(&probe_format);
        return;
    }

    const int video_stream_index = av_find_best_stream(probe_format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_stream_index >= 0) {
        AVStream *video_stream = probe_format->streams[video_stream_index];
        const AVCodecParameters *codecpar = video_stream ? video_stream->codecpar : nullptr;
        std::vector<uint8_t> normalized_extradata;
        if (codecpar && codecpar->codec_id == AV_CODEC_ID_H264 && codecpar->extradata && codecpar->extradata_size > 0) {
            if (!normalizeH264ExtradataForMp4(
                    &normalized_extradata,
                    codecpar->extradata,
                    static_cast<size_t>(codecpar->extradata_size),
                    codecpar,
                    {}
                )) {
                assignExtradataSnapshot(&normalized_extradata, codecpar->extradata, codecpar->extradata_size);
            }
        } else if (codecpar) {
            assignExtradataSnapshot(&normalized_extradata, codecpar->extradata, codecpar->extradata_size);
        }

        AWESOME_FF_LOGI(
            "Output probe video stream: codec=%s codec_tag=%s size=%dx%d pix_fmt=%s avg_frame_rate=%d/%d time_base=%d/%d extradata=%s",
            codecpar ? avcodec_get_name(codecpar->codec_id) : "unknown",
            codecpar ? describeCodecTag(codecpar->codec_tag).c_str() : "unknown",
            codecpar ? codecpar->width : 0,
            codecpar ? codecpar->height : 0,
            (codecpar && codecpar->format != AV_PIX_FMT_NONE && av_get_pix_fmt_name(static_cast<AVPixelFormat>(codecpar->format)))
                ? av_get_pix_fmt_name(static_cast<AVPixelFormat>(codecpar->format))
                : "unknown",
            video_stream ? video_stream->avg_frame_rate.num : 0,
            video_stream ? video_stream->avg_frame_rate.den : 0,
            video_stream ? video_stream->time_base.num : 0,
            video_stream ? video_stream->time_base.den : 0,
            describeExtradata(normalized_extradata).c_str()
        );
    } else {
        AWESOME_FF_LOGW("Output probe could not find a video stream.");
    }

    const int audio_stream_index = av_find_best_stream(probe_format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audio_stream_index >= 0) {
        AVStream *audio_stream = probe_format->streams[audio_stream_index];
        const AVCodecParameters *codecpar = audio_stream ? audio_stream->codecpar : nullptr;
        AWESOME_FF_LOGI(
            "Output probe audio stream: codec=%s sample_rate=%d channels=%d bit_rate=%lld extradata_size=%d",
            codecpar ? avcodec_get_name(codecpar->codec_id) : "unknown",
            codecpar ? codecpar->sample_rate : 0,
            codecpar ? codecpar->ch_layout.nb_channels : 0,
            codecpar ? static_cast<long long>(codecpar->bit_rate) : 0LL,
            codecpar ? codecpar->extradata_size : 0
        );
    }

    AWESOME_FF_LOGI(
        "Output probe format: duration_us=%lld bit_rate=%lld streams=%u",
        static_cast<long long>(probe_format->duration),
        static_cast<long long>(probe_format->bit_rate),
        probe_format->nb_streams
    );

    avformat_close_input(&probe_format);
}

int FFmpegMp4TranscoderBase::rebuildVideoEncoderAfterPauseIfNeeded(const char *reason) {
    if (!video_rebuild_after_pause_pending_) return 0;

    if (!shouldRebuildHardwareVideoEncoderAfterPause()) {
        video_rebuild_after_pause_pending_ = false;
        return 0;
    }

    AWESOME_FF_LOGI(
        "%s",
        (reason && reason[0])
            ? reason
            : "Rebuilding hardware video encoder after pause/resume."
    );

    const int ret = rebuildVideoEncoderForResume();
    if (ret < 0) return ret;

    markVideoEncoderRebuilt();
    return 0;
}

int FFmpegMp4TranscoderBase::updateVideoOutputStreamAfterEncoderChange() {
    if (!output_format_ || !video_enc_ctx_ || video_output_index_ < 0) return AVERROR(EINVAL);

    AVStream *output_stream = output_format_->streams[video_output_index_];
    const std::vector<uint8_t> previous_extradata = video_extradata_snapshot_;

    int ret = avcodec_parameters_from_context(output_stream->codecpar, video_enc_ctx_);
    if (ret < 0) return ret;

    output_stream->codecpar->codec_tag = 0;
    if (!output_header_written_ ||
        output_stream->time_base.num <= 0 ||
        output_stream->time_base.den <= 0) {
        output_stream->time_base = video_enc_ctx_->time_base;
    }
    output_stream->avg_frame_rate = video_enc_ctx_->framerate;
    output_stream->sample_aspect_ratio = video_enc_ctx_->sample_aspect_ratio;

    if (video_enc_ctx_->codec_id == AV_CODEC_ID_H264 &&
        output_stream->codecpar &&
        output_stream->codecpar->extradata &&
        output_stream->codecpar->extradata_size > 0) {
        std::vector<uint8_t> current_extradata;
        assignExtradataSnapshot(
            &current_extradata,
            output_stream->codecpar->extradata,
            output_stream->codecpar->extradata_size
        );
        std::vector<uint8_t> normalized_extradata;
        if (normalizeH264ExtradataForMp4(
                &normalized_extradata,
                output_stream->codecpar->extradata,
                static_cast<size_t>(output_stream->codecpar->extradata_size),
                output_stream->codecpar,
                previous_extradata
            ) &&
            normalized_extradata != current_extradata) {
            ret = replaceCodecparExtradata(output_stream->codecpar, normalized_extradata);
            if (ret < 0) return ret;
            AWESOME_FF_LOGI(
                "Normalized rebuilt H.264 extradata for MP4 stream: %s",
                describeExtradata(normalized_extradata).c_str()
            );
        }
    }

    assignExtradataSnapshot(
        &video_extradata_snapshot_,
        output_stream->codecpar->extradata,
        output_stream->codecpar->extradata_size
    );

    AWESOME_FF_LOGI(
        "Synced video stream params after encoder change: codec=%s time_base=%d/%d frame_rate=%d/%d pix_fmt=%s extradata=%s",
        video_enc_ctx_->codec ? FFmpegCommonUtils::codecNameOrUnknown(video_enc_ctx_->codec) : "unknown",
        output_stream->time_base.num,
        output_stream->time_base.den,
        output_stream->avg_frame_rate.num,
        output_stream->avg_frame_rate.den,
        av_get_pix_fmt_name(video_enc_ctx_->pix_fmt) ? av_get_pix_fmt_name(video_enc_ctx_->pix_fmt) : "unknown",
        describeExtradata(video_extradata_snapshot_).c_str()
    );

    if (video_packets_written_ &&
        previous_extradata != video_extradata_snapshot_ &&
        !video_muxer_reopen_required_) {
        video_pending_extradata_refresh_ = true;
        video_attach_extradata_to_next_packet_ = true;
        video_current_extradata_observed_ = !video_extradata_snapshot_.empty();
        AWESOME_FF_LOGW(
            "Video encoder extradata changed after rebuild. Old %s, new %s. Queued an MP4 extradata refresh and will wait for the first resumed sync packet.",
            describeExtradata(previous_extradata).c_str(),
            describeExtradata(video_extradata_snapshot_).c_str()
        );
    } else if (video_packets_written_ && previous_extradata == video_extradata_snapshot_) {
        video_current_extradata_observed_ = !video_extradata_snapshot_.empty();
        AWESOME_FF_LOGI(
            "Video encoder rebuild kept the same extradata: %s",
            describeExtradata(video_extradata_snapshot_).c_str()
        );
    } else if (!video_packets_written_) {
        video_current_extradata_observed_ = !video_extradata_snapshot_.empty();
    }

    return 0;
}

int FFmpegMp4TranscoderBase::sendVideoFrameWithRecovery(
    AVFrame *frame,
    int64_t absolute_pts,
    const char *operation
) {
    if (!frame || !video_enc_ctx_ || video_output_index_ < 0) return AVERROR(EINVAL);

    const char *op = operation && operation[0] ? operation : "video encode";
    int ret = 0;

    for (int attempt = 0; attempt < 2; ++attempt) {
        video_last_write_packet_count_ = 0;
        prepareVideoFrameForEncoding(frame, absolute_pts);

        ret = avcodec_send_frame(video_enc_ctx_, frame);
        if (ret >= 0) {
            ret = writeEncodedPackets(video_enc_ctx_, video_output_index_);
        }
        if (ret >= 0) return 0;

        if (attempt > 0 ||
            !FFmpegCommonUtils::shouldAttemptVideoEncoderRecovery(video_enc_ctx_, ret) ||
            video_last_write_packet_count_ > 0) {
            return ret;
        }

        char error_buffer[128];
        if (av_strerror(ret, error_buffer, sizeof(error_buffer)) < 0) {
            snprintf(error_buffer, sizeof(error_buffer), "error %d", ret);
        }

        AWESOME_FF_LOGW(
            "%s failed on hardware encoder (%s). Rebuilding and retrying current frame.",
            op,
            error_buffer
        );

        ret = rebuildVideoEncoderForResume();
        if (ret < 0) return ret;

        markVideoEncoderRebuilt();
    }

    return ret;
}

int FFmpegMp4TranscoderBase::writeEncodedPackets(AVCodecContext *encoder_context, int output_stream_index) {
    int ret = 0;
    AVStream *output_stream = output_format_->streams[output_stream_index];
    const bool is_video_output = encoder_context == video_enc_ctx_ && output_stream_index == video_output_index_;
    int consecutive_empty_video_packets = 0;

    video_last_write_packet_count_ = 0;

    while (true) {
        const int pause_ret = waitIfPaused();
        if (pause_ret < 0) return pause_ret;
        if (is_video_output && video_rebuild_after_pause_pending_) {
            AWESOME_FF_LOGW(
                "Pause/resume invalidated the active VideoToolbox session. Rebuilding encoder and retrying current frame."
            );
            return AVERROR(EIO);
        }

        ret = avcodec_receive_packet(encoder_context, enc_packet_);
        if (ret < 0) break;
        av_packet_rescale_ts(enc_packet_, encoder_context->time_base, output_stream->time_base);
        if (is_video_output) {
            const bool should_log_video_packet_debug =
                output_stream &&
                output_stream->codecpar &&
                output_stream->codecpar->codec_id == AV_CODEC_ID_H264 &&
                video_debug_packets_logged_ < 5;
            const int packet_debug_index = video_debug_packets_logged_;
            std::string video_packet_layout_before_conversion;
            if (should_log_video_packet_debug) {
                video_packet_layout_before_conversion = describeH264PacketPayload(
                    enc_packet_,
                    output_stream->codecpar
                );
            }
            if (output_stream &&
                output_stream->codecpar &&
                output_stream->codecpar->codec_id == AV_CODEC_ID_H264) {
                const int packet_format_ret = convertH264PacketFromAnnexBToAvcc(
                    enc_packet_,
                    output_stream->codecpar
                );
                if (packet_format_ret < 0) {
                    av_packet_unref(enc_packet_);
                    return packet_format_ret;
                }
            }

            std::vector<uint8_t> runtime_extradata;
            VideoExtradataSource runtime_extradata_source = kVideoExtradataSourceNone;
            refreshVideoExtradataSnapshotFromRuntimeState(
                &runtime_extradata,
                encoder_context,
                enc_packet_,
                output_stream ? output_stream->codecpar : nullptr,
                video_extradata_snapshot_,
                &runtime_extradata_source
            );
            if (!runtime_extradata.empty() && runtime_extradata_source != kVideoExtradataSourceNone) {
                video_current_extradata_observed_ = true;
            }
            if (!runtime_extradata.empty() && runtime_extradata != video_extradata_snapshot_) {
                const std::vector<uint8_t> previous_extradata = video_extradata_snapshot_;
                video_extradata_snapshot_ = runtime_extradata;
                if (video_packets_written_) {
                    video_pending_extradata_refresh_ = true;
                    video_attach_extradata_to_next_packet_ = true;
                    AWESOME_FF_LOGW(
                        "Observed video extradata change while receiving packets. Old %s, new %s.",
                        describeExtradata(previous_extradata).c_str(),
                        describeExtradata(video_extradata_snapshot_).c_str()
                    );
                    AWESOME_FF_LOGI(
                        "Current video extradata source during resume: %s",
                        describeVideoExtradataSource(runtime_extradata_source)
                    );
                } else {
                    video_pending_extradata_refresh_ = true;
                    video_attach_extradata_to_next_packet_ = true;
                    AWESOME_FF_LOGI(
                        "Captured initial video extradata from runtime state: %s",
                        describeExtradata(video_extradata_snapshot_).c_str()
                    );
                    AWESOME_FF_LOGI(
                        "Initial video extradata source: %s",
                        describeVideoExtradataSource(runtime_extradata_source)
                    );
                    AWESOME_FF_LOGI(
                        "Queued initial video extradata for the first video packet."
                    );
                }
            } else if (video_require_fresh_extradata_ &&
                !runtime_extradata.empty() &&
                runtime_extradata_source != kVideoExtradataSourceNone) {
                AWESOME_FF_LOGI(
                    "Revalidated current video extradata from %s: %s",
                    describeVideoExtradataSource(runtime_extradata_source),
                    describeExtradata(runtime_extradata).c_str()
                );
            }
            if (!video_packets_written_ && !video_extradata_snapshot_.empty()) {
                const int sync_extradata_ret = syncH264StreamExtradataFromSnapshot(
                    output_stream,
                    video_extradata_snapshot_,
                    "initial-runtime-state"
                );
                if (sync_extradata_ret < 0) {
                    av_packet_unref(enc_packet_);
                    return sync_extradata_ret;
                }
            }
            if (!enc_packet_->data || enc_packet_->size <= 0) {
                if (!video_extradata_snapshot_.empty()) {
                    video_pending_extradata_refresh_ = true;
                    video_attach_extradata_to_next_packet_ = true;
                }
                consecutive_empty_video_packets += 1;
                if (should_log_video_packet_debug) {
                    logVideoPacketDebugSummary(
                        packet_debug_index,
                        video_packet_layout_before_conversion,
                        enc_packet_,
                        output_stream->codecpar
                    );
                    video_debug_packets_logged_ = packet_debug_index + 1;
                }
                AWESOME_FF_LOGW(
                    "Dropping empty encoded video packet: pts=%lld dts=%lld side_extradata_pending=%d snapshot=%s",
                    static_cast<long long>(enc_packet_->pts),
                    static_cast<long long>(enc_packet_->dts),
                    video_attach_extradata_to_next_packet_ ? 1 : 0,
                    describeExtradata(video_extradata_snapshot_).c_str()
                );
                if (!video_packets_written_ && consecutive_empty_video_packets >= 8) {
                    AWESOME_FF_LOGW(
                        "Received %d consecutive empty video packets before the first muxed video sample. Triggering encoder recovery.",
                        consecutive_empty_video_packets
                    );
                    av_packet_unref(enc_packet_);
                    return AVERROR(EIO);
                }
                av_packet_unref(enc_packet_);
                continue;
            }
            consecutive_empty_video_packets = 0;
            if (enc_packet_->dts == AV_NOPTS_VALUE && enc_packet_->pts != AV_NOPTS_VALUE) {
                enc_packet_->dts = enc_packet_->pts;
            }
            if (enc_packet_->pts == AV_NOPTS_VALUE && enc_packet_->dts != AV_NOPTS_VALUE) {
                enc_packet_->pts = enc_packet_->dts;
            }

            const int64_t minimum_next_dts = minimumNextVideoDts(output_stream, video_last_written_dts_);
            const bool is_resume_sync_packet = packetIsVideoResumeSyncPacket(
                enc_packet_,
                output_stream,
                encoder_context
            );

            if (video_encoder_output_offset_pts_ == AV_NOPTS_VALUE &&
                minimum_next_dts != AV_NOPTS_VALUE &&
                enc_packet_->dts != AV_NOPTS_VALUE &&
                enc_packet_->dts < minimum_next_dts) {
                video_encoder_output_offset_pts_ = AV_NOPTS_VALUE;
                video_attach_extradata_to_next_packet_ = false;
                video_require_fresh_extradata_ = true;
                video_current_extradata_observed_ = runtime_extradata_source != kVideoExtradataSourceNone;

                if (!is_resume_sync_packet) {
                    AWESOME_FF_LOGW(
                        "Detected internal VideoToolbox timestamp reset before a sync packet. Rebuilding encoder and retrying current frame."
                    );
                    av_packet_unref(enc_packet_);
                    return AVERROR(EIO);
                }
            }

            if (video_drop_until_sync_packet_ && !is_resume_sync_packet) {
                AWESOME_FF_LOGW(
                    "Dropping resumed video packet until the first sync packet arrives after encoder rebuild."
                );
                av_packet_unref(enc_packet_);
                continue;
            }

            if (video_require_fresh_extradata_ && !video_current_extradata_observed_) {
                video_force_keyframe_on_next_frame_ = true;
                video_drop_until_sync_packet_ = true;
                AWESOME_FF_LOGW(
                    "First resumed sync packet arrived, but fresh encoder extradata is still unavailable. Dropping packet and requesting another keyframe."
                );
                av_packet_unref(enc_packet_);
                continue;
            }

            if (video_encoder_output_offset_pts_ == AV_NOPTS_VALUE) {
                int64_t target_start = 0;
                if (video_encoder_input_origin_pts_ != AV_NOPTS_VALUE) {
                    target_start = av_rescale_q(
                        video_encoder_input_origin_pts_,
                        encoder_context->time_base,
                        output_stream->time_base
                    );
                }
                if (minimum_next_dts != AV_NOPTS_VALUE && target_start < minimum_next_dts) {
                    target_start = minimum_next_dts;
                }

                video_encoder_output_offset_pts_ = target_start - packetTimestampAnchor(enc_packet_);
                AWESOME_FF_LOGI(
                    "Video rebuild timestamp rebase: origin_pts=%lld packet_anchor=%lld minimum_next_dts=%lld target_start=%lld offset=%lld",
                    static_cast<long long>(video_encoder_input_origin_pts_),
                    static_cast<long long>(packetTimestampAnchor(enc_packet_)),
                    static_cast<long long>(minimum_next_dts),
                    static_cast<long long>(target_start),
                    static_cast<long long>(video_encoder_output_offset_pts_)
                );
            }

            shiftPacketTimestamps(enc_packet_, video_encoder_output_offset_pts_);
            if (minimum_next_dts != AV_NOPTS_VALUE &&
                enc_packet_->dts != AV_NOPTS_VALUE &&
                enc_packet_->dts < minimum_next_dts) {
                const int64_t shift = minimum_next_dts - enc_packet_->dts;
                video_encoder_output_offset_pts_ += shift;
                shiftPacketTimestamps(enc_packet_, shift);
                AWESOME_FF_LOGW(
                    "Detected video encoder timestamp reset. Rebasing continued output by %lld ticks (target dts=%lld).",
                    static_cast<long long>(shift)
                    , static_cast<long long>(minimum_next_dts)
                );
            }

            if (enc_packet_->dts == AV_NOPTS_VALUE && minimum_next_dts != AV_NOPTS_VALUE) {
                enc_packet_->dts = minimum_next_dts;
            }
            if (enc_packet_->pts == AV_NOPTS_VALUE && enc_packet_->dts != AV_NOPTS_VALUE) {
                enc_packet_->pts = enc_packet_->dts;
            }
            if (enc_packet_->pts != AV_NOPTS_VALUE &&
                enc_packet_->dts != AV_NOPTS_VALUE &&
                enc_packet_->pts < enc_packet_->dts) {
                enc_packet_->pts = enc_packet_->dts;
            }
            if (enc_packet_->pts != AV_NOPTS_VALUE &&
                video_last_written_pts_ != AV_NOPTS_VALUE &&
                enc_packet_->pts <= video_last_written_pts_) {
                int64_t minimum_pts = video_last_written_pts_ + 1;
                if (enc_packet_->dts != AV_NOPTS_VALUE && minimum_pts < enc_packet_->dts) {
                    minimum_pts = enc_packet_->dts;
                }
                enc_packet_->pts = minimum_pts;
            }
            if (video_require_fresh_extradata_ && video_current_extradata_observed_) {
                video_attach_extradata_to_next_packet_ = !video_extradata_snapshot_.empty();
            }

            if (video_attach_extradata_to_next_packet_) {
                if (!video_extradata_snapshot_.empty()) {
                    const int prepend_ret = prependVideoExtradataParameterSetsToPacket(
                        enc_packet_,
                        video_extradata_snapshot_,
                        output_stream ? output_stream->codecpar : nullptr
                    );
                    if (prepend_ret < 0) {
                        av_packet_unref(enc_packet_);
                        return prepend_ret;
                    }
                    if (prepend_ret > 0) {
                        AWESOME_FF_LOGI("Prepended H.264 SPS/PPS to the next video packet.");
                    }

                    size_t side_size = 0;
                    uint8_t *side = av_packet_get_side_data(
                        enc_packet_,
                        AV_PKT_DATA_NEW_EXTRADATA,
                        &side_size
                    );
                    const bool needs_side_data = !side ||
                        side_size != video_extradata_snapshot_.size() ||
                        memcmp(side, video_extradata_snapshot_.data(), side_size) != 0;
                    const int extradata_ret = needs_side_data
                        ? appendPacketExtradataSideData(enc_packet_, video_extradata_snapshot_)
                        : 0;
                    if (extradata_ret < 0) {
                        av_packet_unref(enc_packet_);
                        return extradata_ret;
                    }
                    video_attach_extradata_to_next_packet_ = false;
                    video_pending_extradata_refresh_ = false;
                    AWESOME_FF_LOGI("Attached video extradata to the next video packet.");
                } else {
                    AWESOME_FF_LOGW(
                        "The next video packet is ready, but encoder extradata is still empty."
                    );
                }
            }

            if (should_log_video_packet_debug) {
                logVideoPacketDebugSummary(
                    packet_debug_index,
                    video_packet_layout_before_conversion,
                    enc_packet_,
                    output_stream->codecpar
                );
                video_debug_packets_logged_ = packet_debug_index + 1;
            }
        }

        enc_packet_->stream_index = output_stream_index;
        enc_packet_->pos = -1;

        const bool first_video_packet_write = is_video_output && !video_packets_written_;
        const int64_t packet_pts_before_write = enc_packet_->pts;
        const int64_t packet_dts_before_write = enc_packet_->dts;
        const int64_t packet_duration_before_write = enc_packet_->duration;
        const int packet_size_before_write = enc_packet_->size;
        const int packet_key_before_write = (enc_packet_->flags & AV_PKT_FLAG_KEY) ? 1 : 0;
        size_t packet_extradata_side_size_before_write = 0;
        uint8_t *packet_extradata_side_before_write = av_packet_get_side_data(
            enc_packet_,
            AV_PKT_DATA_NEW_EXTRADATA,
            &packet_extradata_side_size_before_write
        );
        const int packet_has_extradata_side_before_write =
            packet_extradata_side_before_write && packet_extradata_side_size_before_write > 0 ? 1 : 0;
        const int packet_is_sync_before_write = is_video_output && packetIsVideoResumeSyncPacket(
            enc_packet_,
            output_stream,
            encoder_context
        ) ? 1 : 0;

        const int write_ret = av_interleaved_write_frame(output_format_, enc_packet_);
        if (write_ret >= 0 && is_video_output) {
            video_packets_written_ = true;
            video_last_write_packet_count_ += 1;
            if (first_video_packet_write) {
                AWESOME_FF_LOGI(
                    "Wrote first video packet: pts=%lld dts=%lld duration=%lld key=%d sync=%d size=%d extradata_side=%d extradata_pending=%d codec=%s",
                    static_cast<long long>(packet_pts_before_write),
                    static_cast<long long>(packet_dts_before_write),
                    static_cast<long long>(packet_duration_before_write),
                    packet_key_before_write,
                    packet_is_sync_before_write,
                    packet_size_before_write,
                    packet_has_extradata_side_before_write,
                    video_attach_extradata_to_next_packet_ ? 1 : 0,
                    encoder_context->codec ? FFmpegCommonUtils::codecNameOrUnknown(encoder_context->codec) : "unknown"
                );
            }
            video_last_written_pts_ = packet_pts_before_write != AV_NOPTS_VALUE
                ? packet_pts_before_write
                : packet_dts_before_write;
            video_last_written_dts_ = packet_dts_before_write != AV_NOPTS_VALUE
                ? packet_dts_before_write
                : packet_pts_before_write;
            video_drop_until_sync_packet_ = false;
            if (video_current_extradata_observed_) {
                video_require_fresh_extradata_ = false;
            }
        }
        av_packet_unref(enc_packet_);
        if (write_ret < 0) {
            if (is_video_output && video_muxer_reopen_required_) {
                AWESOME_FF_LOGW(
                    "Muxer rejected continued video packets after encoder rebuild. Current stream would require muxer reopen."
                );
            }
            return write_ret;
        }
    }

    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return 0;
    return ret;
}
