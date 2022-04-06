//
// Created by 阳坤 on 2022/3/22.
//

#ifndef FFPLAY2_FF_FFPLAY_H
#define FFPLAY2_FF_FFPLAY_H

#include "ff_sync_renderer.h"

/**
 * 创建播放器
 * @return
 */
struct FFplayer2 *ffp2_create();





/**
 * 销毁播放器
 * @param ffp2
 */
void ffp2_destroy(struct FFplayer2 *ffp2);

/**
 * 播放器初始化
 * @param ffp2
 * @return
 */
int ffp2_init(struct FFplayer2 *ffp2);

/**
 * 开始播放
 * @param ffp2
 */
int ffp2_start(struct FFplayer2 *ffp2);

/**
 * 暂停或者恢复播放
 * @param ffp2
 * @return
 */
void ffp2_pause(struct FFplayer2 *ffp2);

/**
 * 开启音频静音
 * @param ffp2
 */
void ffp2_mute(struct FFplayer2 *ffp2);

/**
 * 更新音频音量
 */
void ffp2_update_volume(struct FFplayer2 *ffp2, int sign, double step);

/**
 * 逐帧播放
 * @param ffp2
 */
void ffp2_step_to_next_frame(struct FFplayer2 *ffp2);

#endif //FFPLAY2_FF_FFPLAY_H
