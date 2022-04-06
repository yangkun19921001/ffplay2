//
// Created by 阳坤 on 2021/11/29.
//

#ifndef FFPLAY2_FF_FRAME_QUEUE_H
#define FFPLAY2_FF_FRAME_QUEUE_H

#include "ff_struct_core.h"

/**
 * 初始化 FrameQueue
 * @param f 原始数据
 * @param pktq 编码数据
 * @param max_size 最大缓存
 * @param keep_last
 * @return
 */
int ff_frame_queue_init(struct FrameQueue *f,struct PacketQueue *pktq, int max_size, int keep_last);

/**
 * 销毁队列中的所有帧
 * @param f
 * @return
 */
int ff_frame_queue_destory(struct FrameQueue *f);

/**
 * 释放缓存帧的引用
 * @param vp
 */
void ff_frame_queue_unref_item(struct Frame *vp);

/**
 * 发送唤醒的信号
 * @param f
 */
void ff_frame_queue_signal(struct FrameQueue *f);

/**
 * 获取队列当前Frame, 在调用该函数前先调用 ff_frame_queue_nb_remaining 确保有frame可读
 * @param f
 * @return
 */
struct Frame *ff_frame_queue_peek(struct FrameQueue *f);

/**
 * 获取当前Frame的下一Frame, 此时要确保queue里面至少有2个Frame
 * @param f
 * @return  不管什么时候调用，返回来肯定不是 NULL
 */
struct Frame *ff_frame_queue_peek_next(struct FrameQueue *f);

/**
 * 获取上⼀Frame
 * 当rindex_shown=0时，和frame_queue_peek效果一样
 * 当rindex_shown=1时，读取的是已经显示过的frame
 * @param f
 * @return
 */
struct Frame *ff_frame_queue_peek_last(struct FrameQueue *f);

/**
 * 获取⼀个可写Frame，可以以阻塞或⾮阻塞⽅式进⾏
 * @param f
 * @return
 */
struct Frame *ff_frame_queue_peek_writable(struct FrameQueue *f);

/**
 * 获取⼀个可读Frame，可以以阻塞或⾮阻塞⽅式进⾏
 * @param f
 * @return
 */
struct Frame *ff_frame_queue_peek_readable(struct FrameQueue *f);

/**
 * 更新写索引，此时Frame才真正⼊队列，队列节点Frame个数加1
 * @param f
 */
void ff_frame_queue_push(struct FrameQueue *f);

/**
 * 更新读索引，此时Frame才真正出队列，队列节点Frame个数减1，内部调⽤
 * 当keep_last为1, rindex_show为0时不去更新rindex,也不释放当前frame
 * @param f
 */
void ff_frame_queue_next(struct FrameQueue *f);

/**
 * 确保⾄少有2 Frame在队列
 * @param f
 * @return
 */
int ff_frame_queue_nb_remaining(struct FrameQueue *f);

/**
 * 获取最近播放Frame对应数据在媒体⽂件的位置，主要在seek时使⽤
 * @param f
 * @return
 */
int64_t ff_frame_queue_last_pos(struct FrameQueue *f);


#endif //FFPLAY2_FF_FRAME_QUEUE_H
