//
// Created by 阳坤 on 2022/3/20.
//

#ifndef FFPLAY2_FF_DEMUXER_H
#define FFPLAY2_FF_DEMUXER_H

#include "ff_struct_core.h"

/**
 * 解封装初始化
 * @return 返回 视频状态维护
 */
int ff_demuxer_init(struct FFplayer2 *fp);

/**
 * 开始解封装
 * @param is
 */
int ff_demuxer_start(struct FFplayer2 *fp);

/**
 * 销毁
 * @param is
 */
void ff_demuxer_destory(struct FFplayer2 *fp);

/**
 * seek 操作
 * @param is
 * @param pos
 * @param rel
 * @param seek_by_bytes
 */
void ff_demuxer_stream_seek(struct FFplayer2 *ff, int64_t pos, int64_t rel, int seek_by_bytes);


/**
 * 暂停->播放
 * @param is
 */
void ff_demuxer_stream_pause(struct FFplayer2*ff);
#endif //FFPLAY2_FF_DEMUXER_H
