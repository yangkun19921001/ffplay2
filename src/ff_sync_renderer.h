//
// Created by 阳坤 on 2022/3/22.
//主要是对音视频同步后的渲染
//

#ifndef FFPLAY2_FF_SYNC_RENDERER_H
#define FFPLAY2_FF_SYNC_RENDERER_H

#include "ff_audio_sdl_renderer.h"
#include "ff_video_sdl_renderer.h"
#include "ff_demuxer.h"
#include "ff_decoder.h"
/**
 * 音视频同步渲染 初始化
 * @param ffp2
 * @return
 */
int ff_sync_renderer_init(struct FFplayer2 *ffp2);

/**
 * 音视频同步渲染 开始
 * @param ffp2
 * @return
 */
int ff_sync_renderer_start(struct FFplayer2 *ffp2);

/**
 * 音视频同步渲染 销毁
 * @param ffp2
 * @return
 */
int ff_sync_renderer_destory(struct FFplayer2 *ffp2);

/**
 * 视频循环刷新
 * @param ffp2
 * @param event
 */
void ff_refresh_loop_wait_event(struct FFplayer2 *ffp2, SDL_Event *event);

#endif //FFPLAY2_FF_SYNC_RENDERER_H
