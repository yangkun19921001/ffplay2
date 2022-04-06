//
// Created by 阳坤 on 2022/3/22.
//

#include "ff_ffplay.h"

struct FFplayer2 *ffp2_create() {
    VideoState *is = NULL;
    struct FFplayer2 *ffPlayer = NULL;
    if (!(is = (av_malloc(sizeof(VideoState)))))
        return NULL;
    memset(is, 0, sizeof(VideoState));
    if (!(ffPlayer = (av_malloc(sizeof(struct FFplayer2)))))
        return NULL;
    memset(ffPlayer, 0, sizeof(struct FFplayer2));
    ffPlayer->is = is;
    if (!(ffPlayer->ffOptions = (av_malloc(sizeof(struct FFOptions)))))
        return NULL;
    memset(ffPlayer->ffOptions, 0, sizeof(struct FFOptions));
    initFFOptions(ffPlayer->ffOptions);
    return ffPlayer;
}

int ffp2_init(struct FFplayer2 *ffp2) {
    if (!ffp2 || !ffp2->is || !ffp2->is->filename)return AVERROR(EOBJNULL);
    /* 初始化帧队列 */
    if (ff_frame_queue_init(&ffp2->is->pictq, &ffp2->is->videoq, VIDEO_PICTURE_QUEUE_SIZE, 1) < 0)
        goto fail;
    if (ff_frame_queue_init(&ffp2->is->subpq, &ffp2->is->subtitleq, SUBPICTURE_QUEUE_SIZE, 0) < 0)
        goto fail;
    if (ff_frame_queue_init(&ffp2->is->sampq, &ffp2->is->audioq, SAMPLE_QUEUE_SIZE, 1) < 0)
        goto fail;

    if (ff_packet_queue_init(&ffp2->is->videoq) < 0 ||
        ff_packet_queue_init(&ffp2->is->audioq) < 0 ||
        ff_packet_queue_init(&ffp2->is->subtitleq) < 0)
        goto fail;

    //必须首先初始化，内部会初始化解码缓冲队列
    int ret = ff_sync_renderer_init(ffp2);
    if (ret < 0)
        goto fail;

    //解封装初始化
    ret = ff_demuxer_init(ffp2);
    if (ret < 0)
        goto fail;

    //解码初始化,内部会开启解码线程
    ret = ff_decoder_init(ffp2);
    if (ret < 0)
        goto fail;
    ret = 0;
    fail:
    return ret;
}

int ffp2_start(struct FFplayer2 *ffp2) {
    int ret = ff_sync_renderer_start(ffp2);
    if (ret < 0)
        goto fail;
    ret = ff_demuxer_start(ffp2);
    if (ret < 0)
        goto fail;
    ret = 0;
    fail:
    return ret;
}


void ffp2_destroy(struct FFplayer2 *ffp2) {
    if (!ffp2)return;
    if (ffp2->is) {
        // 动态(线程/callback)的先停止退出
        /* XXX: use a special url_shutdown call to abort parse cleanly */
        ffp2->is->abort_request = 1;                // 请求退出
        SDL_WaitThread(ffp2->is->read_tid, NULL);    // 等待数据读取线程退出
        ff_decoder_destroy(ffp2);
        ff_demuxer_destory(ffp2);
        SDL_DestroyCond(ffp2->is->continue_read_thread);
        ff_sync_renderer_destory(ffp2);
        av_free(ffp2->is);
        ffp2->is = NULL;
    }
}

void ffp2_pause(struct FFplayer2 *ffp2) {
    ff_demuxer_stream_pause(ffp2);
    ffp2->is->step = 0;   // 逐帧的时候用
    printf("is->step = 0; toggle_pause\n");
}

void ffp2_mute(struct FFplayer2 *ffp2) {
    ffp2->is->muted = !ffp2->is->muted;
}


void ffp2_update_volume(struct FFplayer2 *ffp2, int sign, double step) {

    double volume_level = ffp2->is->audio_volume ? (20 * log(ffp2->is->audio_volume / (double) SDL_MIX_MAXVOLUME) / log(10))
                                                 : -1000.0;
    int new_volume = lrint(SDL_MIX_MAXVOLUME * pow(10.0, (volume_level + sign * step) / 20.0));
    ffp2->is->audio_volume = av_clip(ffp2->is->audio_volume == new_volume ? (ffp2->is->audio_volume + sign) : new_volume, 0,
                               SDL_MIX_MAXVOLUME);
    printf("update_volume audio_volume:%d\n", ffp2->is->audio_volume);

}


void ffp2_step_to_next_frame(struct FFplayer2 *ffp2) {
    /* if the stream is paused unpause it, then step */
    if (ffp2->is->paused)
        ff_demuxer_stream_pause(ffp2);
    ffp2->is->step = 1;
    printf("is->step = 1; step_to_next_frame\n");
}
