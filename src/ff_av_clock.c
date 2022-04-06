//
// Created by 阳坤 on 2022/3/20.
//

#include "ff_av_clock.h"


void ff_init_clock(struct Clock *c, int *queue_serial) {
    c->speed = 1.0;
    c->paused = 0;
    c->queue_serial = queue_serial;
    ff_set_clock(c, NAN, -1);
}

void ff_set_clock(struct Clock *c, double pts, int serial) {
    double time = av_gettime_relative() / 1000000.0;
    ff_set_clock_at(c, pts, serial, time);
}

void ff_set_clock_at(struct Clock *c, double pts, int serial, double time) {
    c->pts = pts;                           /* 当前帧的pts */
    c->last_updated = time;                 /* 最后更新的时间，实际上是当前的一个系统时间 */
    c->pts_drift = c->pts - time;           /* 当前帧pts和系统时间的差值，正常播放情况下两者的差值应该是比较固定的，因为两者都是以时间为基准进行线性增长 */
    c->serial = serial;
}

double ff_get_clock(struct Clock *c) {
    if (*c->queue_serial != c->serial)
        return NAN; // 不是同一个播放序列，时钟是无效
    if (c->paused) {
        return c->pts;  // 暂停的时候返回的是pts
    } else {
        double time = av_gettime_relative() / 1000000.0;
        return c->pts_drift + time - (time - c->last_updated) * (1.0 - c->speed);
    }
}

void ff_set_clock_speed(struct Clock *c, double speed) {
    ff_set_clock(c, ff_get_clock(c), c->serial);
    c->speed = speed;
}

void ff_sync_clock_to_slave(struct Clock *c,struct  Clock *slave) {
    double clock = ff_get_clock(c);
    double slave_clock = ff_get_clock(slave);
    if (!isnan(slave_clock) && (isnan(clock) || fabs(clock - slave_clock) > AV_NOSYNC_THRESHOLD))
        ff_set_clock(c, slave_clock, slave->serial);
}

int ff_get_master_sync_type(struct VideoState *is) {
    if (is->av_sync_type == AV_SYNC_VIDEO_MASTER) {
        if (is->video_st)
            return AV_SYNC_VIDEO_MASTER;
        else
            return AV_SYNC_AUDIO_MASTER;     /* 如果没有视频成分则使用 audio master */
    } else if (is->av_sync_type == AV_SYNC_AUDIO_MASTER) {
        if (is->audio_st)
            return AV_SYNC_AUDIO_MASTER;
        else
            return AV_SYNC_EXTERNAL_CLOCK;     /* 没有音频的时候那就用外部时钟 */
    } else {
        return AV_SYNC_EXTERNAL_CLOCK;
    }
}

double ff_get_master_clock(struct VideoState *is) {
    double val;

    switch (ff_get_master_sync_type(is)) {
        case AV_SYNC_VIDEO_MASTER:
            val = ff_get_clock(&is->vidclk);
            break;
        case AV_SYNC_AUDIO_MASTER:
            val = ff_get_clock(&is->audclk);
            break;
        default:
            val = ff_get_clock(&is->extclk);
            break;
    }
    return val;
}

void ff_check_external_clock_speed(struct VideoState *is) {
    if (is->video_stream >= 0 && is->videoq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES ||
        is->audio_stream >= 0 && is->audioq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES) {
        ff_set_clock_speed(&is->extclk, FFMAX(EXTERNAL_CLOCK_SPEED_MIN, is->extclk.speed - EXTERNAL_CLOCK_SPEED_STEP));
    } else if ((is->video_stream < 0 || is->videoq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES) &&
               (is->audio_stream < 0 || is->audioq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES)) {
        ff_set_clock_speed(&is->extclk, FFMIN(EXTERNAL_CLOCK_SPEED_MAX, is->extclk.speed + EXTERNAL_CLOCK_SPEED_STEP));
    } else {
        double speed = is->extclk.speed;
        if (speed != 1.0)
            ff_set_clock_speed(&is->extclk, speed + EXTERNAL_CLOCK_SPEED_STEP * (1.0 - speed) / fabs(1.0 - speed));
    }
}
