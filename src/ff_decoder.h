//
// Created by 阳坤 on 2022/3/21.
//

#ifndef FFPLAY2_FF_DECODER_H
#define FFPLAY2_FF_DECODER_H

#include "ff_struct_core.h"
#include "ff_frame_queue.h"
#include "ff_av_clock.h"
#include "ff_packet_queue.h"
/**
 * 解码器器初始化
 * @param fp2
 * @return
 */
int ff_decoder_init(struct FFplayer2 *fp2);

/**
 * 解码销毁
 * @param d
 */
void ff_decoder_destroy(struct FFplayer2 *fp2);

/**
 * 打开解码器组件
 * @brief stream_component_open
 * @param is
 * @param stream_index 流索引
 * @return Return 0 if OK
 */
int ff_stream_component_open(struct FFplayer2 *ff, int stream_index);

/**
 * 关闭解码器组件
 * @param is
 * @param stream_index
 */
void ff_stream_component_close(struct FFplayer2 *ff, int stream_index);

/**
 * 创建解码线程, audio/video有独立的线程
 */
int ff_decoder_start(struct Decoder *d, int (*fn)(void *), const char *thread_name, void *arg);

/**
 * 解码停止
 * @param d
 * @param fq
 */
void ff_decoder_abort(struct Decoder *d, struct FrameQueue *fq);

#endif //FFPLAY2_FF_DECODER_H
