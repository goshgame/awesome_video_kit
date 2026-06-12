#include "ffmpeg_watermark_filter_utils.h"

#include <utility>
#include <vector>

#include <zlib.h>

namespace {

constexpr uint32_t makePngChunkType(char a, char b, char c, char d) {
    return (static_cast<uint32_t>(static_cast<uint8_t>(a)) << 24) |
        (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 16) |
        (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 8) |
        static_cast<uint32_t>(static_cast<uint8_t>(d));
}

constexpr uint32_t kPngChunkTypeCgBI = makePngChunkType('C', 'g', 'B', 'I');
constexpr uint32_t kPngChunkTypeIHDR = makePngChunkType('I', 'H', 'D', 'R');
constexpr uint32_t kPngChunkTypeIDAT = makePngChunkType('I', 'D', 'A', 'T');
constexpr uint32_t kPngChunkTypeIEND = makePngChunkType('I', 'E', 'N', 'D');

struct MemoryImageReader {
    const uint8_t *data;
    size_t size;
    size_t offset;
};

struct PngChunkData {
    uint32_t type;
    std::vector<uint8_t> data;
};

uint32_t readBigEndianUint32(const uint8_t *data) {
    return (static_cast<uint32_t>(data[0]) << 24) |
        (static_cast<uint32_t>(data[1]) << 16) |
        (static_cast<uint32_t>(data[2]) << 8) |
        static_cast<uint32_t>(data[3]);
}

void appendBigEndianUint32(std::vector<uint8_t> *buffer, uint32_t value) {
    if (!buffer) return;
    buffer->push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    buffer->push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    buffer->push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    buffer->push_back(static_cast<uint8_t>(value & 0xFF));
}

void appendPngChunk(std::vector<uint8_t> *buffer, uint32_t type, const uint8_t *data, size_t size) {
    if (!buffer || size > UINT32_MAX) return;

    const uint8_t type_bytes[4] = {
        static_cast<uint8_t>((type >> 24) & 0xFF),
        static_cast<uint8_t>((type >> 16) & 0xFF),
        static_cast<uint8_t>((type >> 8) & 0xFF),
        static_cast<uint8_t>(type & 0xFF),
    };

    appendBigEndianUint32(buffer, static_cast<uint32_t>(size));
    buffer->insert(buffer->end(), type_bytes, type_bytes + sizeof(type_bytes));
    if (data && size > 0) {
        buffer->insert(buffer->end(), data, data + size);
    }

    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, type_bytes, sizeof(type_bytes));
    if (data && size > 0) {
        crc = crc32(crc, reinterpret_cast<const Bytef *>(data), static_cast<uInt>(size));
    }
    appendBigEndianUint32(buffer, static_cast<uint32_t>(crc));
}

int clampByte(int value) {
    if (value < 0) return 0;
    if (value > 255) return 255;
    return value;
}

int paethPredictor(int left, int up, int up_left) {
    const int p = left + up - up_left;
    const int pa = abs(p - left);
    const int pb = abs(p - up);
    const int pc = abs(p - up_left);

    if (pa <= pb && pa <= pc) return left;
    if (pb <= pc) return up;
    return up_left;
}

int readMemoryImageData(void *opaque, uint8_t *buf, int buf_size) {
    MemoryImageReader *reader = static_cast<MemoryImageReader *>(opaque);
    size_t remaining = 0;
    size_t bytes_to_copy = 0;

    if (!reader || !buf || buf_size <= 0) return AVERROR(EINVAL);
    if (reader->offset >= reader->size) return AVERROR_EOF;

    remaining = reader->size - reader->offset;
    bytes_to_copy = remaining < static_cast<size_t>(buf_size) ? remaining : static_cast<size_t>(buf_size);
    memcpy(buf, reader->data + reader->offset, bytes_to_copy);
    reader->offset += bytes_to_copy;
    return static_cast<int>(bytes_to_copy);
}

int64_t seekMemoryImageData(void *opaque, int64_t offset, int whence) {
    MemoryImageReader *reader = static_cast<MemoryImageReader *>(opaque);
    int origin = whence & ~AVSEEK_FORCE;
    int64_t base = 0;
    int64_t target = 0;

    if (!reader) return AVERROR(EINVAL);
    if (whence == AVSEEK_SIZE) return static_cast<int64_t>(reader->size);

    switch (origin) {
        case SEEK_SET:
            base = 0;
            break;
        case SEEK_CUR:
            base = static_cast<int64_t>(reader->offset);
            break;
        case SEEK_END:
            base = static_cast<int64_t>(reader->size);
            break;
        default:
            return AVERROR(EINVAL);
    }

    target = base + offset;
    if (target < 0 || target > static_cast<int64_t>(reader->size)) {
        return AVERROR(EINVAL);
    }

    reader->offset = static_cast<size_t>(target);
    return target;
}

int decodeFirstVideoFrameFromFormatContext(
    AVFormatContext *format_context,
    AVFrame **frame_out,
    AVRational *time_base_out
) {
    AVCodecContext *decode_context = nullptr;
    const AVCodec *decoder = nullptr;
    AVStream *stream = nullptr;
    AVFrame *frame = nullptr;
    AVPacket packet = {0};
    int stream_index = -1;
    int ret = 0;

    if (!format_context || !frame_out) return AVERROR(EINVAL);
    *frame_out = nullptr;
    if (time_base_out) *time_base_out = AVRational{1, 25};

    ret = avformat_find_stream_info(format_context, nullptr);
    if (ret < 0) goto end;

    stream_index = av_find_best_stream(format_context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (stream_index < 0) {
        ret = stream_index;
        goto end;
    }

    stream = format_context->streams[stream_index];
    decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!decoder) {
        ret = AVERROR_DECODER_NOT_FOUND;
        goto end;
    }

    decode_context = avcodec_alloc_context3(decoder);
    if (!decode_context) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    ret = avcodec_parameters_to_context(decode_context, stream->codecpar);
    if (ret < 0) goto end;
    decode_context->pkt_timebase = stream->time_base;

    ret = avcodec_open2(decode_context, decoder, nullptr);
    if (ret < 0) goto end;

    frame = av_frame_alloc();
    if (!frame) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    while ((ret = av_read_frame(format_context, &packet)) >= 0) {
        if (packet.stream_index != stream_index) {
            av_packet_unref(&packet);
            continue;
        }

        ret = avcodec_send_packet(decode_context, &packet);
        av_packet_unref(&packet);
        if (ret < 0) goto end;

        while ((ret = avcodec_receive_frame(decode_context, frame)) >= 0) {
            frame->pts = 0;
            if (frame->sample_aspect_ratio.num <= 0 || frame->sample_aspect_ratio.den <= 0) {
                frame->sample_aspect_ratio = AVRational{1, 1};
            }
            *frame_out = frame;
            frame = nullptr;
            if (time_base_out && stream->time_base.num > 0 && stream->time_base.den > 0) {
                *time_base_out = stream->time_base;
            }
            ret = 0;
            goto end;
        }

        if (ret == AVERROR(EAGAIN)) continue;
        if (ret < 0 && ret != AVERROR_EOF) goto end;
    }

    if (ret == AVERROR_EOF || ret >= 0) {
        ret = avcodec_send_packet(decode_context, nullptr);
        if (ret < 0 && ret != AVERROR_EOF) goto end;

        while ((ret = avcodec_receive_frame(decode_context, frame)) >= 0) {
            frame->pts = 0;
            if (frame->sample_aspect_ratio.num <= 0 || frame->sample_aspect_ratio.den <= 0) {
                frame->sample_aspect_ratio = AVRational{1, 1};
            }
            *frame_out = frame;
            frame = nullptr;
            if (time_base_out && stream && stream->time_base.num > 0 && stream->time_base.den > 0) {
                *time_base_out = stream->time_base;
            }
            ret = 0;
            goto end;
        }
    }

    if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) {
        ret = AVERROR_INVALIDDATA;
    }

end:
    av_packet_unref(&packet);
    if (frame) av_frame_free(&frame);
    if (decode_context) avcodec_free_context(&decode_context);
    return ret;
}

int decodeImageBufferToFrame(
    const uint8_t *data,
    size_t size,
    const char *input_format_name,
    AVFrame **frame_out,
    AVRational *time_base_out
) {
    AVFormatContext *format_context = nullptr;
    AVIOContext *io_context = nullptr;
    uint8_t *io_buffer = nullptr;
    MemoryImageReader reader = {data, size, 0};
    const AVInputFormat *input_format = nullptr;
    int ret = 0;

    if (!data || size == 0 || !frame_out) return AVERROR(EINVAL);
    *frame_out = nullptr;
    if (time_base_out) *time_base_out = AVRational{1, 25};

    io_buffer = static_cast<uint8_t *>(av_malloc(4096));
    if (!io_buffer) return AVERROR(ENOMEM);

    io_context = avio_alloc_context(
        io_buffer,
        4096,
        0,
        &reader,
        &readMemoryImageData,
        nullptr,
        &seekMemoryImageData
    );
    if (!io_context) {
        av_free(io_buffer);
        return AVERROR(ENOMEM);
    }
    io_buffer = nullptr;

    format_context = avformat_alloc_context();
    if (!format_context) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    format_context->pb = io_context;
    format_context->flags |= AVFMT_FLAG_CUSTOM_IO;

    if (input_format_name && *input_format_name) {
        input_format = av_find_input_format(input_format_name);
    }

    ret = avformat_open_input(&format_context, nullptr, input_format, nullptr);
    if (ret < 0) goto end;

    ret = decodeFirstVideoFrameFromFormatContext(format_context, frame_out, time_base_out);

end:
    if (format_context) {
        AVIOContext *owned_io_context = format_context->pb;
        avformat_close_input(&format_context);
        if (owned_io_context) {
            av_freep(&owned_io_context->buffer);
            avio_context_free(&owned_io_context);
        }
    } else if (io_context) {
        av_freep(&io_context->buffer);
        avio_context_free(&io_context);
    }
    return ret;
}

int decodeRawImageDataToFrame(
    enum AVCodecID codec_id,
    const uint8_t *data,
    size_t size,
    AVFrame **frame_out,
    AVRational *time_base_out
) {
    const AVCodec *decoder = nullptr;
    AVCodecContext *decode_context = nullptr;
    AVFrame *frame = nullptr;
    AVPacket packet = {0};
    int ret = 0;

    if (!data || size == 0 || !frame_out) return AVERROR(EINVAL);
    if (size > INT_MAX) return AVERROR(EFBIG);

    *frame_out = nullptr;
    if (time_base_out) *time_base_out = AVRational{1, 25};

    decoder = avcodec_find_decoder(codec_id);
    if (!decoder) return AVERROR_DECODER_NOT_FOUND;

    decode_context = avcodec_alloc_context3(decoder);
    if (!decode_context) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    ret = avcodec_open2(decode_context, decoder, nullptr);
    if (ret < 0) goto end;

    frame = av_frame_alloc();
    if (!frame) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    ret = av_new_packet(&packet, static_cast<int>(size));
    if (ret < 0) goto end;
    memcpy(packet.data, data, size);

    ret = avcodec_send_packet(decode_context, &packet);
    av_packet_unref(&packet);
    if (ret < 0) goto end;

    ret = avcodec_receive_frame(decode_context, frame);
    if (ret < 0) goto end;

    frame->pts = 0;
    if (frame->sample_aspect_ratio.num <= 0 || frame->sample_aspect_ratio.den <= 0) {
        frame->sample_aspect_ratio = AVRational{1, 1};
    }

    *frame_out = frame;
    frame = nullptr;
    ret = 0;

end:
    av_packet_unref(&packet);
    if (frame) av_frame_free(&frame);
    if (decode_context) avcodec_free_context(&decode_context);
    return ret;
}

int bufferHasPngChunk(const uint8_t *data, size_t size, uint32_t chunk_type) {
    size_t offset = 8;

    if (!data || size < 8) return 0;

    while (offset + 12 <= size) {
        const uint32_t chunk_length = readBigEndianUint32(data + offset);
        const size_t chunk_end = offset + 12 + static_cast<size_t>(chunk_length);
        const uint32_t type = readBigEndianUint32(data + offset + 4);

        if (chunk_end > size) return 0;
        if (type == chunk_type) return 1;
        if (type == kPngChunkTypeIEND) return 0;

        offset = chunk_end;
    }

    return 0;
}

int convertCgbiPngToStandardPng(
    const uint8_t *data,
    size_t size,
    std::vector<uint8_t> *output
) {
    static const uint8_t kPngSignature[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<PngChunkData> chunks_before_idat;
    std::vector<PngChunkData> chunks_after_idat;
    std::vector<uint8_t> idat_payload;
    std::vector<uint8_t> decompressed_scanlines;
    std::vector<uint8_t> unfiltered_rows;
    std::vector<uint8_t> refiltered_scanlines;
    std::vector<uint8_t> recompressed_idat;
    size_t offset = sizeof(kPngSignature);
    uint32_t width = 0;
    uint32_t height = 0;
    int bit_depth = 0;
    int color_type = 0;
    int interlace_method = 0;
    int seen_idat = 0;
    int saw_cgbi = 0;
    int ret = 0;

    if (!data || size < sizeof(kPngSignature) || !output) return AVERROR(EINVAL);
    if (memcmp(data, kPngSignature, sizeof(kPngSignature)) != 0) return AVERROR_INVALIDDATA;

    while (offset + 12 <= size) {
        const uint32_t chunk_length = readBigEndianUint32(data + offset);
        const uint32_t chunk_type = readBigEndianUint32(data + offset + 4);
        const uint8_t *chunk_data = data + offset + 8;
        const size_t next_offset = offset + 12 + static_cast<size_t>(chunk_length);

        if (next_offset > size) return AVERROR_INVALIDDATA;

        if (chunk_type == kPngChunkTypeCgBI) {
            saw_cgbi = 1;
        } else if (chunk_type == kPngChunkTypeIHDR) {
            PngChunkData chunk = {chunk_type, std::vector<uint8_t>(chunk_data, chunk_data + chunk_length)};
            if (chunk_length != 13) return AVERROR_INVALIDDATA;
            width = readBigEndianUint32(chunk_data);
            height = readBigEndianUint32(chunk_data + 4);
            bit_depth = chunk_data[8];
            color_type = chunk_data[9];
            interlace_method = chunk_data[12];
            chunks_before_idat.push_back(std::move(chunk));
        } else if (chunk_type == kPngChunkTypeIDAT) {
            seen_idat = 1;
            idat_payload.insert(idat_payload.end(), chunk_data, chunk_data + chunk_length);
        } else if (chunk_type == kPngChunkTypeIEND) {
            break;
        } else {
            PngChunkData chunk = {chunk_type, std::vector<uint8_t>(chunk_data, chunk_data + chunk_length)};
            if (seen_idat) {
                chunks_after_idat.push_back(std::move(chunk));
            } else {
                chunks_before_idat.push_back(std::move(chunk));
            }
        }

        offset = next_offset;
    }

    if (!saw_cgbi) return AVERROR_INVALIDDATA;
    if (width == 0 || height == 0 || idat_payload.empty()) return AVERROR_INVALIDDATA;
    if (bit_depth != 8) return AVERROR_PATCHWELCOME;
    if (interlace_method != 0) return AVERROR_PATCHWELCOME;
    if (color_type != 2 && color_type != 6) return AVERROR_PATCHWELCOME;

    const int channels = color_type == 6 ? 4 : 3;
    const size_t bytes_per_pixel = static_cast<size_t>(channels);
    const size_t row_bytes = static_cast<size_t>(width) * bytes_per_pixel;
    const size_t filtered_row_bytes = row_bytes + 1;
    const size_t expected_size = static_cast<size_t>(height) * filtered_row_bytes;

    if (row_bytes == 0 || filtered_row_bytes < row_bytes) return AVERROR_INVALIDDATA;
    if (height > 0 && expected_size / filtered_row_bytes != static_cast<size_t>(height)) {
        return AVERROR_INVALIDDATA;
    }

    decompressed_scanlines.resize(expected_size);
    {
        z_stream stream;
        memset(&stream, 0, sizeof(stream));
        stream.next_in = const_cast<Bytef *>(reinterpret_cast<const Bytef *>(idat_payload.data()));
        stream.avail_in = static_cast<uInt>(idat_payload.size());
        stream.next_out = reinterpret_cast<Bytef *>(decompressed_scanlines.data());
        stream.avail_out = static_cast<uInt>(decompressed_scanlines.size());

        ret = inflateInit2(&stream, -15);
        if (ret != Z_OK) return AVERROR_INVALIDDATA;

        ret = inflate(&stream, Z_FINISH);
        inflateEnd(&stream);
        if (ret != Z_STREAM_END || stream.total_out != decompressed_scanlines.size()) {
            return AVERROR_INVALIDDATA;
        }
    }

    unfiltered_rows.resize(static_cast<size_t>(height) * row_bytes);
    for (uint32_t row = 0; row < height; ++row) {
        const uint8_t *src = decompressed_scanlines.data() + static_cast<size_t>(row) * filtered_row_bytes;
        uint8_t *dst = unfiltered_rows.data() + static_cast<size_t>(row) * row_bytes;
        const uint8_t *prev = row > 0 ? (unfiltered_rows.data() + static_cast<size_t>(row - 1) * row_bytes) : nullptr;
        const uint8_t filter_type = src[0];

        for (size_t i = 0; i < row_bytes; ++i) {
            const int raw = src[i + 1];
            const int left = i >= bytes_per_pixel ? dst[i - bytes_per_pixel] : 0;
            const int up = prev ? prev[i] : 0;
            const int up_left = (prev && i >= bytes_per_pixel) ? prev[i - bytes_per_pixel] : 0;
            int value = 0;

            switch (filter_type) {
                case 0:
                    value = raw;
                    break;
                case 1:
                    value = raw + left;
                    break;
                case 2:
                    value = raw + up;
                    break;
                case 3:
                    value = raw + ((left + up) >> 1);
                    break;
                case 4:
                    value = raw + paethPredictor(left, up, up_left);
                    break;
                default:
                    return AVERROR_INVALIDDATA;
            }

            dst[i] = static_cast<uint8_t>(value & 0xFF);
        }
    }

    for (uint32_t row = 0; row < height; ++row) {
        uint8_t *row_data = unfiltered_rows.data() + static_cast<size_t>(row) * row_bytes;
        for (uint32_t column = 0; column < width; ++column) {
            uint8_t *pixel = row_data + static_cast<size_t>(column) * bytes_per_pixel;
            const uint8_t blue = pixel[0];
            const uint8_t red = pixel[2];
            pixel[0] = red;
            pixel[2] = blue;

            if (channels == 4) {
                const int alpha = pixel[3];
                if (alpha == 0) {
                    pixel[0] = 0;
                    pixel[1] = 0;
                    pixel[2] = 0;
                } else if (alpha < 255) {
                    pixel[0] = static_cast<uint8_t>(clampByte((pixel[0] * 255 + alpha / 2) / alpha));
                    pixel[1] = static_cast<uint8_t>(clampByte((pixel[1] * 255 + alpha / 2) / alpha));
                    pixel[2] = static_cast<uint8_t>(clampByte((pixel[2] * 255 + alpha / 2) / alpha));
                }
            }
        }
    }

    refiltered_scanlines.resize(expected_size);
    for (uint32_t row = 0; row < height; ++row) {
        uint8_t *dst = refiltered_scanlines.data() + static_cast<size_t>(row) * filtered_row_bytes;
        const uint8_t *src = unfiltered_rows.data() + static_cast<size_t>(row) * row_bytes;
        dst[0] = 0;
        memcpy(dst + 1, src, row_bytes);
    }

    recompressed_idat.resize(compressBound(static_cast<uLong>(refiltered_scanlines.size())));
    {
        uLongf compressed_size = static_cast<uLongf>(recompressed_idat.size());
        ret = compress2(
            reinterpret_cast<Bytef *>(recompressed_idat.data()),
            &compressed_size,
            reinterpret_cast<const Bytef *>(refiltered_scanlines.data()),
            static_cast<uLong>(refiltered_scanlines.size()),
            Z_BEST_COMPRESSION
        );
        if (ret != Z_OK) return AVERROR_EXTERNAL;
        recompressed_idat.resize(static_cast<size_t>(compressed_size));
    }

    output->clear();
    output->insert(output->end(), kPngSignature, kPngSignature + sizeof(kPngSignature));
    for (const PngChunkData &chunk : chunks_before_idat) {
        appendPngChunk(output, chunk.type, chunk.data.empty() ? nullptr : chunk.data.data(), chunk.data.size());
    }
    appendPngChunk(
        output,
        kPngChunkTypeIDAT,
        recompressed_idat.empty() ? nullptr : recompressed_idat.data(),
        recompressed_idat.size()
    );
    for (const PngChunkData &chunk : chunks_after_idat) {
        appendPngChunk(output, chunk.type, chunk.data.empty() ? nullptr : chunk.data.data(), chunk.data.size());
    }
    appendPngChunk(output, kPngChunkTypeIEND, nullptr, 0);
    return 0;
}

void buildFixedOverlayPositionExpr(
    FFmpegWatermarkPosition position,
    int margin_x,
    int margin_y,
    char *x_expr,
    size_t x_expr_size,
    char *y_expr,
    size_t y_expr_size
) {
    switch (position) {
        case FFmpegWatermarkPositionTopLeft:
            snprintf(x_expr, x_expr_size, "%d", margin_x);
            snprintf(y_expr, y_expr_size, "%d", margin_y);
            break;
        case FFmpegWatermarkPositionTopRight:
            snprintf(x_expr, x_expr_size, "main_w-overlay_w-%d", margin_x);
            snprintf(y_expr, y_expr_size, "%d", margin_y);
            break;
        case FFmpegWatermarkPositionBottomLeft:
            snprintf(x_expr, x_expr_size, "%d", margin_x);
            snprintf(y_expr, y_expr_size, "main_h-overlay_h-%d", margin_y);
            break;
        case FFmpegWatermarkPositionCenter:
            snprintf(x_expr, x_expr_size, "(main_w-overlay_w)/2");
            snprintf(y_expr, y_expr_size, "(main_h-overlay_h)/2");
            break;
        case FFmpegWatermarkPositionBottomRight:
        default:
            snprintf(x_expr, x_expr_size, "main_w-overlay_w-%d", margin_x);
            snprintf(y_expr, y_expr_size, "main_h-overlay_h-%d", margin_y);
            break;
    }
}

void buildAlternatingTopLeftBottomRightExpr(
    int margin_x,
    int margin_y,
    char *x_expr,
    size_t x_expr_size,
    char *y_expr,
    size_t y_expr_size
) {
    snprintf(
        x_expr,
        x_expr_size,
        "if(lt(mod(trunc(t/5),2),1),%d,main_w-overlay_w-%d)",
        margin_x,
        margin_x
    );
    snprintf(
        y_expr,
        y_expr_size,
        "if(lt(mod(trunc(t/5),2),1),%d,main_h-overlay_h-%d)",
        margin_y,
        margin_y
    );
}

}  // namespace

int FFmpegWatermarkFilterUtils::isEnabled(const FFmpegWatermarkConfig *watermark) {
    return watermark && watermark->image_path && watermark->image_path[0] != '\0';
}

void FFmpegWatermarkFilterUtils::buildOverlayPositionExpr(
    FFmpegWatermarkPosition position,
    int margin_x,
    int margin_y,
    char *x_expr,
    size_t x_expr_size,
    char *y_expr,
    size_t y_expr_size
) {
    const int effective_margin_x = margin_x > 0 ? margin_x : 20;
    const int effective_margin_y = margin_y > 0 ? margin_y : 20;

    if (position == FFmpegWatermarkPositionAlternatingTopLeftBottomRight) {
        buildAlternatingTopLeftBottomRightExpr(
            effective_margin_x,
            effective_margin_y,
            x_expr,
            x_expr_size,
            y_expr,
            y_expr_size
        );
        return;
    }

    buildFixedOverlayPositionExpr(
        position,
        effective_margin_x,
        effective_margin_y,
        x_expr,
        x_expr_size,
        y_expr,
        y_expr_size
    );
}

int FFmpegWatermarkFilterUtils::computeAutoWatermarkBound(int video_side) {
    if (video_side <= 0) return 0;

    int bound = (video_side * 18) / 100;
    if (bound <= 0) bound = 1;
    if (bound < 32) bound = video_side < 32 ? video_side : 32;
    return bound;
}

int FFmpegWatermarkFilterUtils::shouldScaleWatermark(
    const FFmpegWatermarkConfig *watermark,
    const AVFrame *watermark_frame,
    int auto_max_width,
    int auto_max_height
) {
    if (!watermark || !watermark_frame) return 0;

    if (watermark->width > 0 || watermark->height > 0) return 1;

    return auto_max_width > 0 &&
        auto_max_height > 0 &&
        (watermark_frame->width > auto_max_width || watermark_frame->height > auto_max_height);
}

void FFmpegWatermarkFilterUtils::buildWatermarkScaleArgs(
    const FFmpegWatermarkConfig *watermark,
    int auto_max_width,
    int auto_max_height,
    char *scale_args,
    size_t scale_args_size
) {
    if (!scale_args || scale_args_size == 0) return;

    if (watermark && (watermark->width > 0 || watermark->height > 0)) {
        snprintf(
            scale_args,
            scale_args_size,
            "w=%d:h=%d",
            watermark->width > 0 ? watermark->width : -1,
            watermark->height > 0 ? watermark->height : -1
        );
        return;
    }

    snprintf(
        scale_args,
        scale_args_size,
        "w=%d:h=%d:force_original_aspect_ratio=decrease",
        auto_max_width > 0 ? auto_max_width : -1,
        auto_max_height > 0 ? auto_max_height : -1
    );
}

int FFmpegWatermarkFilterUtils::readFileToBuffer(const char *path, uint8_t **data_out, size_t *size_out) {
    FILE *file = nullptr;
    uint8_t *data = nullptr;
    long file_size = 0;
    size_t bytes_read = 0;
    int ret = 0;

    if (!path || !*path || !data_out || !size_out) return AVERROR(EINVAL);
    *data_out = nullptr;
    *size_out = 0;

    file = fopen(path, "rb");
    if (!file) return AVERROR(errno ? errno : ENOENT);

    if (fseek(file, 0, SEEK_END) != 0) {
        ret = AVERROR(errno ? errno : EIO);
        goto end;
    }

    file_size = ftell(file);
    if (file_size < 0) {
        ret = AVERROR(errno ? errno : EIO);
        goto end;
    }
    if (file_size == 0) {
        ret = AVERROR_INVALIDDATA;
        goto end;
    }
    if (file_size > INT_MAX) {
        ret = AVERROR(EFBIG);
        goto end;
    }

    if (fseek(file, 0, SEEK_SET) != 0) {
        ret = AVERROR(errno ? errno : EIO);
        goto end;
    }

    data = static_cast<uint8_t *>(av_malloc(static_cast<size_t>(file_size) + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!data) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    bytes_read = fread(data, 1, static_cast<size_t>(file_size), file);
    if (bytes_read != static_cast<size_t>(file_size)) {
        ret = AVERROR(errno ? errno : EIO);
        goto end;
    }

    memset(data + file_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    *data_out = data;
    *size_out = static_cast<size_t>(file_size);
    data = nullptr;

end:
    if (file) fclose(file);
    if (data) av_free(data);
    return ret;
}

int FFmpegWatermarkFilterUtils::bufferIsPng(const uint8_t *data, size_t size) {
    static const uint8_t kPngSignature[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    return data &&
        size >= sizeof(kPngSignature) &&
        memcmp(data, kPngSignature, sizeof(kPngSignature)) == 0;
}

int FFmpegWatermarkFilterUtils::bufferIsJpeg(const uint8_t *data, size_t size) {
    return data &&
        size >= 3 &&
        data[0] == 0xFF &&
        data[1] == 0xD8 &&
        data[2] == 0xFF;
}

int FFmpegWatermarkFilterUtils::decodePngDataToFrame(
    const uint8_t *data,
    size_t size,
    AVFrame **frame_out,
    AVRational *time_base_out
) {
    if (!data || size == 0 || !frame_out) return AVERROR(EINVAL);
    int ret = decodeImageBufferToFrame(data, size, "png_pipe", frame_out, time_base_out);
    if (ret >= 0) return 0;
    return decodeRawImageDataToFrame(AV_CODEC_ID_PNG, data, size, frame_out, time_base_out);
}

int FFmpegWatermarkFilterUtils::decodeJpegDataToFrame(
    const uint8_t *data,
    size_t size,
    AVFrame **frame_out,
    AVRational *time_base_out
) {
    if (!data || size == 0 || !frame_out) return AVERROR(EINVAL);
    int ret = decodeImageBufferToFrame(data, size, "jpeg_pipe", frame_out, time_base_out);
    if (ret >= 0) return 0;
    ret = decodeImageBufferToFrame(data, size, "mjpeg", frame_out, time_base_out);
    if (ret >= 0) return 0;
    return decodeRawImageDataToFrame(AV_CODEC_ID_MJPEG, data, size, frame_out, time_base_out);
}

int FFmpegWatermarkFilterUtils::decodeImageFileToFrame(
    const char *image_path,
    AVFrame **frame_out,
    AVRational *time_base_out
) {
    AVFormatContext *format_context = nullptr;
    int ret = 0;
    uint8_t *image_data = nullptr;
    size_t image_size = 0;
    std::vector<uint8_t> converted_png_buffer;

    if (!image_path || !*image_path || !frame_out) return AVERROR(EINVAL);
    *frame_out = nullptr;
    if (time_base_out) *time_base_out = AVRational{1, 25};

    ret = avformat_open_input(&format_context, image_path, nullptr, nullptr);
    if (ret < 0) {
        const AVInputFormat *image_input_format = av_find_input_format("image2");
        if (image_input_format) {
            ret = avformat_open_input(&format_context, image_path, image_input_format, nullptr);
        }
    }
    if (ret >= 0) {
        ret = decodeFirstVideoFrameFromFormatContext(format_context, frame_out, time_base_out);
        if (ret >= 0) goto end;
    }

    ret = readFileToBuffer(image_path, &image_data, &image_size);
    if (ret < 0 || !image_data || image_size == 0) goto end;

    if (bufferIsPng(image_data, image_size)) {
        if (bufferHasPngChunk(image_data, image_size, kPngChunkTypeCgBI)) {
            ret = convertCgbiPngToStandardPng(image_data, image_size, &converted_png_buffer);
            if (ret >= 0 && !converted_png_buffer.empty()) {
                ret = decodePngDataToFrame(
                    converted_png_buffer.data(),
                    converted_png_buffer.size(),
                    frame_out,
                    time_base_out
                );
                if (ret >= 0) goto end;
            }
        }

        ret = decodePngDataToFrame(image_data, image_size, frame_out, time_base_out);
        if (ret >= 0) goto end;
    } else if (bufferIsJpeg(image_data, image_size)) {
        ret = decodeJpegDataToFrame(image_data, image_size, frame_out, time_base_out);
        if (ret >= 0) goto end;
    }

end:
    if (image_data) av_free(image_data);
    if (format_context) avformat_close_input(&format_context);
    return ret;
}

#if !AWESOME_HAS_AVFILTER
int FFmpegWatermarkFilterUtils::initializeWatermarkFilterGraph(
    AVCodecContext *,
    AVCodecContext *,
    AVStream *,
    int,
    int,
    const FFmpegWatermarkConfig *,
    AVFilterGraph **,
    AVFilterContext **,
    AVFilterContext **,
    AVRational *
) {
    return AVERROR(ENOSYS);
}
#else
int FFmpegWatermarkFilterUtils::initializeWatermarkFilterGraph(
    AVCodecContext *video_dec_ctx,
    AVCodecContext *video_enc_ctx,
    AVStream *video_input_stream,
    int output_width,
    int output_height,
    const FFmpegWatermarkConfig *watermark,
    AVFilterGraph **filter_graph_out,
    AVFilterContext **buffer_src_ctx_out,
    AVFilterContext **buffer_sink_ctx_out,
    AVRational *filter_time_base_out
) {
    int ret = 0;
    char main_args[512];
    char watermark_args[512];
    char overlay_args[256];
    char main_scale_args[128];
    char watermark_scale_args[128];
    char x_expr[128];
    char y_expr[128];
    AVFilterGraph *filter_graph = nullptr;
    AVFilterContext *main_buffer_src_ctx = nullptr;
    AVFilterContext *main_scale_ctx = nullptr;
    AVFilterContext *watermark_buffer_src_ctx = nullptr;
    AVFilterContext *watermark_scale_ctx = nullptr;
    AVFilterContext *buffer_sink_ctx = nullptr;
    AVFilterContext *overlay_ctx = nullptr;
    const AVFilter *buffer_src = avfilter_get_by_name("buffer");
    const AVFilter *buffer_sink = avfilter_get_by_name("buffersink");
    const AVFilter *overlay = avfilter_get_by_name("overlay");
    const AVFilter *scale = avfilter_get_by_name("scale");
    AVRational sample_aspect_ratio;
    AVRational packet_time_base;
    AVRational watermark_time_base = AVRational{1, 25};
    AVFrame *watermark_frame = nullptr;
    const int auto_max_width = computeAutoWatermarkBound(output_width > 0 ? output_width : video_dec_ctx->width);
    const int auto_max_height = computeAutoWatermarkBound(output_height > 0 ? output_height : video_dec_ctx->height);
    const int needs_main_scale = output_width > 0 &&
        output_height > 0 &&
        (output_width != video_dec_ctx->width || output_height != video_dec_ctx->height);
    AVFilterContext *main_video_tail = nullptr;
    AVFilterContext *watermark_tail = nullptr;
    AVRational filter_time_base = AVRational{0, 1};

    if (!video_dec_ctx || !video_enc_ctx || !video_input_stream || !watermark ||
        !filter_graph_out || !buffer_src_ctx_out || !buffer_sink_ctx_out || !filter_time_base_out) {
        return AVERROR(EINVAL);
    }
    if (!buffer_src || !buffer_sink || !overlay) return AVERROR_FILTER_NOT_FOUND;

    *filter_graph_out = nullptr;
    *buffer_src_ctx_out = nullptr;
    *buffer_sink_ctx_out = nullptr;
    *filter_time_base_out = AVRational{0, 1};

    sample_aspect_ratio = video_dec_ctx->sample_aspect_ratio;
    packet_time_base = video_dec_ctx->pkt_timebase;
    if (sample_aspect_ratio.num <= 0 || sample_aspect_ratio.den <= 0) {
        sample_aspect_ratio = AVRational{1, 1};
    }
    if (packet_time_base.num <= 0 || packet_time_base.den <= 0) {
        packet_time_base = video_input_stream->time_base;
    }
    if (packet_time_base.num <= 0 || packet_time_base.den <= 0) {
        packet_time_base = AVRational{1, 25};
    }

    ret = decodeImageFileToFrame(watermark->image_path, &watermark_frame, &watermark_time_base);
    if (ret < 0) goto end;
    if (watermark_time_base.num <= 0 || watermark_time_base.den <= 0) {
        watermark_time_base = AVRational{1, 25};
    }
    if (watermark_frame->sample_aspect_ratio.num <= 0 || watermark_frame->sample_aspect_ratio.den <= 0) {
        watermark_frame->sample_aspect_ratio = AVRational{1, 1};
    }

    filter_graph = avfilter_graph_alloc();
    if (!filter_graph) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    snprintf(
        main_args,
        sizeof(main_args),
        "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
        video_dec_ctx->width,
        video_dec_ctx->height,
        video_dec_ctx->pix_fmt,
        packet_time_base.num,
        packet_time_base.den,
        sample_aspect_ratio.num,
        sample_aspect_ratio.den
    );
    ret = avfilter_graph_create_filter(&main_buffer_src_ctx, buffer_src, "main_in", main_args, nullptr, filter_graph);
    if (ret < 0) goto end;
    main_video_tail = main_buffer_src_ctx;

    if (needs_main_scale) {
        if (!scale) {
            ret = AVERROR_FILTER_NOT_FOUND;
            goto end;
        }
        snprintf(
            main_scale_args,
            sizeof(main_scale_args),
            "w=%d:h=%d:flags=bicubic",
            output_width,
            output_height
        );
        ret = avfilter_graph_create_filter(&main_scale_ctx, scale, "main_scale", main_scale_args, nullptr, filter_graph);
        if (ret < 0) goto end;
        ret = avfilter_link(main_buffer_src_ctx, 0, main_scale_ctx, 0);
        if (ret < 0) goto end;
        main_video_tail = main_scale_ctx;
    }

    snprintf(
        watermark_args,
        sizeof(watermark_args),
        "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
        watermark_frame->width,
        watermark_frame->height,
        watermark_frame->format,
        watermark_time_base.num,
        watermark_time_base.den,
        watermark_frame->sample_aspect_ratio.num,
        watermark_frame->sample_aspect_ratio.den
    );
    ret = avfilter_graph_create_filter(&watermark_buffer_src_ctx, buffer_src, "watermark_in", watermark_args, nullptr, filter_graph);
    if (ret < 0) goto end;
    watermark_tail = watermark_buffer_src_ctx;

    buildOverlayPositionExpr(
        watermark->position,
        watermark->margin_x,
        watermark->margin_y,
        x_expr,
        sizeof(x_expr),
        y_expr,
        sizeof(y_expr)
    );
    snprintf(
        overlay_args,
        sizeof(overlay_args),
        "x=%s:y=%s:eof_action=repeat:repeatlast=1",
        x_expr,
        y_expr
    );
    ret = avfilter_graph_create_filter(&overlay_ctx, overlay, "overlay", overlay_args, nullptr, filter_graph);
    if (ret < 0) goto end;

    ret = avfilter_graph_create_filter(&buffer_sink_ctx, buffer_sink, "out", nullptr, nullptr, filter_graph);
    if (ret < 0) goto end;

    ret = av_opt_set_bin(
        buffer_sink_ctx,
        "pix_fmts",
        reinterpret_cast<uint8_t *>(&video_enc_ctx->pix_fmt),
        sizeof(video_enc_ctx->pix_fmt),
        AV_OPT_SEARCH_CHILDREN
    );
    if (ret < 0) goto end;

    ret = avfilter_link(main_video_tail, 0, overlay_ctx, 0);
    if (ret < 0) goto end;

    if (shouldScaleWatermark(watermark, watermark_frame, auto_max_width, auto_max_height)) {
        if (!scale) {
            ret = AVERROR_FILTER_NOT_FOUND;
            goto end;
        }
        buildWatermarkScaleArgs(
            watermark,
            auto_max_width,
            auto_max_height,
            watermark_scale_args,
            sizeof(watermark_scale_args)
        );
        ret = avfilter_graph_create_filter(
            &watermark_scale_ctx,
            scale,
            "watermark_scale",
            watermark_scale_args,
            nullptr,
            filter_graph
        );
        if (ret < 0) goto end;
        ret = avfilter_link(watermark_buffer_src_ctx, 0, watermark_scale_ctx, 0);
        if (ret < 0) goto end;
        watermark_tail = watermark_scale_ctx;
    }

    ret = avfilter_link(watermark_tail, 0, overlay_ctx, 1);
    if (ret < 0) goto end;
    ret = avfilter_link(overlay_ctx, 0, buffer_sink_ctx, 0);
    if (ret < 0) goto end;

    ret = avfilter_graph_config(filter_graph, nullptr);
    if (ret < 0) goto end;

    watermark_frame->pts = 0;
    ret = av_buffersrc_add_frame_flags(watermark_buffer_src_ctx, watermark_frame, AV_BUFFERSRC_FLAG_KEEP_REF);
    if (ret < 0) goto end;

    ret = av_buffersrc_add_frame_flags(watermark_buffer_src_ctx, nullptr, 0);
    if (ret < 0 && ret != AVERROR_EOF) goto end;

    filter_time_base = av_buffersink_get_time_base(buffer_sink_ctx);
    if (filter_time_base.num <= 0 || filter_time_base.den <= 0) {
        filter_time_base = packet_time_base;
    }

    *filter_graph_out = filter_graph;
    *buffer_src_ctx_out = main_buffer_src_ctx;
    *buffer_sink_ctx_out = buffer_sink_ctx;
    *filter_time_base_out = filter_time_base;
    filter_graph = nullptr;

end:
    if (watermark_frame) av_frame_free(&watermark_frame);
    if (filter_graph) avfilter_graph_free(&filter_graph);
    return ret;
}
#endif
