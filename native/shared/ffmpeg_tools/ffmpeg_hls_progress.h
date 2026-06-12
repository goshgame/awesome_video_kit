//
//  ffmpeg_hls_progress.h
//  AwesomeVideoKitSDK
//
//  Created by dev on 2026/3/20.
//

#ifndef AWESOME_FFMPEG_HLS_PROGRESS_H
#define AWESOME_FFMPEG_HLS_PROGRESS_H

#include "ffmpeg_hls_common.h"

class FFmpegProgressTracker {
public:
    // 创建一个未绑定时长和回调的进度跟踪器。
    FFmpegProgressTracker() {
        reset(0, nullptr, nullptr);
    }

    // 创建一个带总时长和回调的进度跟踪器。
    FFmpegProgressTracker(
        int64_t duration_us,
        void (*cb)(int percentage, void *user_data),
        void *user_data
    ) {
        reset(duration_us, cb, user_data);
    }

    // 重置进度状态，并根据时长切换精确或估算模式。
    void reset(
        int64_t duration_us,
        void (*cb)(int percentage, void *user_data),
        void *user_data
    ) {
        cb_ = cb;
        user_ = user_data;
        last_percent_ = -1;
        start_time_us_ = AV_NOPTS_VALUE;

        if (duration_us > 0) {
            mode_ = Mode::kDuration;
            total_duration_us_ = duration_us;
        } else {
            mode_ = Mode::kEstimate;
            total_duration_us_ = 0;
        }
    }

    // 根据最新时间戳更新进度百分比。
    void update(int64_t pts, AVRational time_base) {
        if (!cb_ || pts == AV_NOPTS_VALUE) return;

        int64_t time_us = av_rescale_q(pts, time_base, AV_TIME_BASE_Q);
        if (start_time_us_ == AV_NOPTS_VALUE) {
            start_time_us_ = time_us;
        }

        int64_t elapsed_us = time_us - start_time_us_;
        if (elapsed_us < 0) elapsed_us = 0;

        int percent = 0;
        if (mode_ == Mode::kDuration) {
            percent = total_duration_us_ > 0
                ? static_cast<int>((elapsed_us * 100) / total_duration_us_)
                : 0;
        } else {
            percent = static_cast<int>(elapsed_us / 1000000);
        }

        if (percent > 99) percent = 99;
        if (percent <= last_percent_) return;

        last_percent_ = percent;
        cb_(percent, user_);
    }

    // 主动把进度补齐到 100%。
    void finish() {
        if (cb_ && last_percent_ < 100) {
            last_percent_ = 100;
            cb_(100, user_);
        }
    }

private:
    // 标记当前使用精确时长还是估算时长模式。
    enum class Mode {
        kDuration = 0,
        kEstimate = 1,
    };

    Mode mode_ = Mode::kEstimate;
    int64_t total_duration_us_ = 0;
    int64_t start_time_us_ = AV_NOPTS_VALUE;
    int last_percent_ = -1;
    void (*cb_)(int percentage, void *user_data) = nullptr;
    void *user_ = nullptr;
};

#endif /* AWESOME_FFMPEG_HLS_PROGRESS_H */
