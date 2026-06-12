//
//  ffmpeg_hls_downloader.cpp
//  AwesomeVideoKitSDK
//
//  Created by dev on 2026/3/20.
//

#include "ffmpeg_hls_downloader.h"

#include "ffmpeg_common_utils.h"
#include "ffmpeg_hls_network.h"
#include "ffmpeg_hls_progress.h"
#include "ffmpeg_hls_watermark_pipeline.h"

namespace {

class FFmpegBitstreamFilter {
public:
    // 创建一个尚未绑定具体 BSF 的包装器。
    FFmpegBitstreamFilter() : context_(nullptr) {}

    // 释放 BSF 上下文，避免残留内部缓存。
    ~FFmpegBitstreamFilter() {
        if (context_) av_bsf_free(&context_);
    }
    // 根据名称初始化码流过滤器，并拷贝输入流参数。
    int initialize(AVStream *stream, const char *name) {
        const AVBitStreamFilter *filter = av_bsf_get_by_name(name);
        if (!filter) return AVERROR_FILTER_NOT_FOUND;

        int ret = av_bsf_alloc(filter, &context_);
        if (ret < 0) return ret;

        ret = avcodec_parameters_copy(context_->par_in, stream->codecpar);
        if (ret < 0) {
            av_bsf_free(&context_);
            return ret;
        }

        context_->time_base_in = stream->time_base;
        ret = av_bsf_init(context_);
        if (ret < 0) {
            av_bsf_free(&context_);
            return ret;
        }
        return 0;
    }

    // 返回底层 BSF 上下文，供发送和接收 packet 使用。
    AVBSFContext *get() const {
        return context_;
    }

private:
    AVBSFContext *context_;
};

class FFmpegNetworkScope {
public:
    // 进入作用域时初始化 FFmpeg 网络模块。
    FFmpegNetworkScope() {
        avformat_network_init();
    }

    // 离开作用域时关闭 FFmpeg 网络模块。
    ~FFmpegNetworkScope() {
        avformat_network_deinit();
    }
};

// 按时间基把 packet 的时间戳转换到目标流。
void rescalePacketTs(AVPacket *packet, AVRational source_time_base, AVRational destination_time_base) {
    if (!packet) return;
    if (source_time_base.num <= 0 || source_time_base.den <= 0) return;
    if (destination_time_base.num <= 0 || destination_time_base.den <= 0) return;
    av_packet_rescale_ts(packet, source_time_base, destination_time_base);
}

// 选择不超过 720p 的最佳视频流，没有则退回最高分辨率流。
int selectVideoStreamMax720pOrHighest(AVFormatContext *input_format) {
    int best_index = -1;
    int best_height = 0;
    int fallback_index = -1;
    int fallback_height = 0;

    for (unsigned int index = 0; index < input_format->nb_streams; ++index) {
        AVStream *stream = input_format->streams[index];
        if (stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) continue;

        const int height = stream->codecpar->height;
        const int width = stream->codecpar->width;
        const int short_edge = height < width ? height : width;

        if (short_edge <= 720 && short_edge > best_height) {
            best_index = static_cast<int>(index);
            best_height = short_edge;
        }
        if (short_edge > fallback_height) {
            fallback_index = static_cast<int>(index);
            fallback_height = short_edge;
        }
    }

    return best_index >= 0 ? best_index : fallback_index;
}

} // namespace

FFmpegHlsDownloader::FFmpegHlsDownloader(
    const char *m3u8_url,
    const char *output_path,
    const FFmpegWatermarkConfig *watermark,
    void (*progress_cb)(int percentage, void *user_data),
    void *user_data,
    volatile int *cancel_flag
) : m3u8_url_(m3u8_url),
    output_path_(output_path),
    watermark_(watermark),
    progress_cb_(progress_cb),
    user_data_(user_data),
    cancel_flag_(cancel_flag) {
    FFmpegCommonUtils::installPlatformLogBridge();
}

// 串联网络解析、封装拷贝和可选水印流程，产出目标 mp4。
int FFmpegHlsDownloader::download() {
    if (!m3u8_url_ || !*m3u8_url_ || !output_path_ || !*output_path_) return AVERROR(EINVAL);

    const int need_watermark = FFmpegWatermarkPipeline::isEnabled(watermark_);
#if !AWESOME_HAS_AVFILTER
    if (need_watermark) {
        AWESOME_FF_LOGE("Watermark requires FFmpeg libavfilter/libswscale support.");
        return AVERROR(ENOSYS);
    }
#endif

    FFmpegResolvedInputUrl resolved_input;
    FFmpegBitstreamFilter audio_bsf;
    FFmpegWatermarkPipeline watermark_pipeline;
    FFmpegProgressTracker progress;
    FFmpegNetworkScope network_scope;

    AVFormatContext *input_format = nullptr;
    AVFormatContext *output_format = nullptr;
    AVDictionary *options = nullptr;
    AVPacket packet = {0};

    int ret = 0;
    int video_input_index = -1;
    int audio_input_index = -1;
    int video_output_index = -1;
    int audio_output_index = -1;
    int64_t duration_us = 0;

    AVStream *video_input_stream = nullptr;
    AVStream *audio_input_stream = nullptr;
    AVStream *video_output_stream = nullptr;
    AVStream *audio_output_stream = nullptr;

    resolved_input = FFmpegHlsNetworkUtils::resolveEffectiveInputUrl(m3u8_url_);

    FFmpegHlsNetworkUtils::setCommonNetworkOptions(&options);
    ret = avformat_open_input(&input_format, resolved_input.full_url.c_str(), nullptr, &options);
    av_dict_free(&options);
    if (ret < 0) goto end;

    ret = avformat_find_stream_info(input_format, nullptr);
    if (ret < 0) goto end;

    video_input_index = selectVideoStreamMax720pOrHighest(input_format);
    audio_input_index = av_find_best_stream(input_format, AVMEDIA_TYPE_AUDIO, -1, video_input_index, nullptr, 0);
    if (video_input_index < 0) {
        ret = AVERROR_STREAM_NOT_FOUND;
        goto end;
    }

    ret = avformat_alloc_output_context2(&output_format, nullptr, "mp4", output_path_);
    if (ret < 0) goto end;

    video_input_stream = input_format->streams[video_input_index];
    video_output_stream = avformat_new_stream(output_format, nullptr);
    if (!video_output_stream) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    video_output_index = video_output_stream->index;

#if AWESOME_HAS_AVFILTER
    if (need_watermark) {
        ret = watermark_pipeline.initialize(
            input_format,
            output_format,
            video_input_index,
            video_output_index,
            watermark_
        );
        if (ret < 0) goto end;
    } else
#endif
    {
        ret = avcodec_parameters_copy(video_output_stream->codecpar, video_input_stream->codecpar);
        if (ret < 0) goto end;
        video_output_stream->codecpar->codec_tag = 0;
        video_output_stream->time_base = video_input_stream->time_base;
    }

    if (audio_input_index >= 0) {
        audio_input_stream = input_format->streams[audio_input_index];
        audio_output_stream = avformat_new_stream(output_format, nullptr);
        if (!audio_output_stream) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
        audio_output_index = audio_output_stream->index;

        ret = avcodec_parameters_copy(audio_output_stream->codecpar, audio_input_stream->codecpar);
        if (ret < 0) goto end;
        audio_output_stream->codecpar->codec_tag = 0;
        audio_output_stream->time_base = audio_input_stream->time_base;

        if (audio_input_stream->codecpar->codec_id == AV_CODEC_ID_AAC &&
            audio_input_stream->codecpar->extradata_size == 0) {
            ret = audio_bsf.initialize(audio_input_stream, "aac_adtstoasc");
            if (ret < 0) {
                FFmpegCommonUtils::printError("Failed to init aac_adtstoasc BSF", ret);
                goto end;
            }
        }
    }

    ret = avio_open(&output_format->pb, output_path_, AVIO_FLAG_WRITE);
    if (ret < 0) goto end;

    ret = avformat_write_header(output_format, nullptr);
    if (ret < 0) goto end;

    duration_us = FFmpegCommonUtils::getDurationInUsSafe(input_format);
    if (duration_us <= 0 && resolved_input.playlist_is_vod && resolved_input.playlist_duration_us > 0) {
        duration_us = resolved_input.playlist_duration_us;
    }
    progress.reset(duration_us, progress_cb_, user_data_);

    while ((ret = av_read_frame(input_format, &packet)) >= 0) {
        if (cancel_flag_ && *cancel_flag_) break;

        const int input_index = packet.stream_index;
        if (input_index == video_input_index) {
#if AWESOME_HAS_AVFILTER
            if (need_watermark) {
                ret = watermark_pipeline.processPacket(&packet, output_format, &progress);
                if (ret < 0) goto end;
                continue;
            }
#endif

            AVStream *output_stream = output_format->streams[video_output_index];
            packet.stream_index = video_output_index;
            rescalePacketTs(&packet, video_input_stream->time_base, output_stream->time_base);
            packet.pos = -1;

            const int64_t timestamp = packet.pts != AV_NOPTS_VALUE ? packet.pts : packet.dts;
            progress.update(timestamp, output_stream->time_base);

            ret = av_interleaved_write_frame(output_format, &packet);
            av_packet_unref(&packet);
            if (ret < 0) {
                FFmpegCommonUtils::printError("Error writing video frame", ret);
                goto end;
            }
            continue;
        }

        if (input_index == audio_input_index && audio_output_index >= 0) {
            AVStream *output_stream = output_format->streams[audio_output_index];
            packet.stream_index = audio_output_index;

            if (audio_bsf.get()) {
                ret = av_bsf_send_packet(audio_bsf.get(), &packet);
                av_packet_unref(&packet);
                if (ret < 0) {
                    FFmpegCommonUtils::printError("aac_adtstoasc av_bsf_send_packet error", ret);
                    goto end;
                }

                while ((ret = av_bsf_receive_packet(audio_bsf.get(), &packet)) == 0) {
                    packet.stream_index = audio_output_index;
                    rescalePacketTs(&packet, audio_bsf.get()->time_base_out, output_stream->time_base);
                    packet.pos = -1;

                    const int write_ret = av_interleaved_write_frame(output_format, &packet);
                    av_packet_unref(&packet);
                    if (write_ret < 0) {
                        FFmpegCommonUtils::printError("Error writing audio frame", write_ret);
                        ret = write_ret;
                        goto end;
                    }
                }

                if (ret == AVERROR(EAGAIN)) continue;
                if (ret == AVERROR_EOF) break;
                if (ret < 0) {
                    FFmpegCommonUtils::printError("aac_adtstoasc av_bsf_receive_packet error", ret);
                    goto end;
                }
                continue;
            }

            if (audio_input_stream && output_stream) {
                rescalePacketTs(&packet, audio_input_stream->time_base, output_stream->time_base);
            }
            packet.pos = -1;

            ret = av_interleaved_write_frame(output_format, &packet);
            av_packet_unref(&packet);
            if (ret < 0) {
                FFmpegCommonUtils::printError("Error writing audio frame", ret);
                goto end;
            }
            continue;
        }

        av_packet_unref(&packet);
    }

    if (ret != AVERROR_EOF && ret < 0) goto end;

    if (!(cancel_flag_ && *cancel_flag_)) {
#if AWESOME_HAS_AVFILTER
        if (need_watermark) {
            ret = watermark_pipeline.flush(output_format, &progress);
            if (ret < 0) goto end;
        }
#endif

        if (audio_bsf.get() && audio_output_index >= 0) {
            AVStream *output_stream = output_format->streams[audio_output_index];
            int flush_ret = av_bsf_send_packet(audio_bsf.get(), nullptr);
            if (flush_ret >= 0) {
                while ((flush_ret = av_bsf_receive_packet(audio_bsf.get(), &packet)) == 0) {
                    packet.stream_index = audio_output_index;
                    rescalePacketTs(&packet, audio_bsf.get()->time_base_out, output_stream->time_base);
                    packet.pos = -1;

                    ret = av_interleaved_write_frame(output_format, &packet);
                    av_packet_unref(&packet);
                    if (ret < 0) goto end;
                }
            }
        }

        av_write_trailer(output_format);
        progress.finish();
        ret = 0;
    } else {
        ret = 0;
    }

end:
    av_packet_unref(&packet);
    if (input_format) avformat_close_input(&input_format);
    if (output_format) {
        if (output_format->pb) avio_closep(&output_format->pb);
        avformat_free_context(output_format);
    }
    return ret;
}
