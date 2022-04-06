//
// Created by 阳坤 on 2021/11/29.
//

#ifndef FFPLAY2_FF_PACKET_QUEUE_H
#define FFPLAY2_FF_PACKET_QUEUE_H

#include "ff_struct_core.h"


/**
 * 初始化各个字段的值
 * @param q
 * @return
 */
int ff_packet_queue_init(struct PacketQueue *q);

/**
 * 将已经存在的节点清除
 * @param q
 */
void ff_packet_queue_flush(struct PacketQueue *q);

/**
 * 消息队列，释放内存
 * @param q
 */
void ff_packet_queue_destroy(struct PacketQueue *q);

/**
 * 中止队列
 * @param q
 */
void ff_packet_queue_abort(struct PacketQueue *q);

/**
 * 启动队列
 * @param q
 */
void ff_packet_queue_start(struct PacketQueue *q);

/**
 * 从队列中取一个节点
 * @param q
 * @param pkt
 * @param block
 * @param serial
 * @return
 */
int ff_packet_queue_get(struct PacketQueue *q, AVPacket *pkt, int block, int *serial);

/**
 * 存放一个节点
 * @param q
 * @param pkt
 * @return
 */
int ff_packet_queue_put(struct PacketQueue *q, AVPacket *pkt);

/**
 * 放入一个空包
 * @param q
 * @param starat_index
 * @return
 */
int ff_packet_queue_put_nullpacket(struct PacketQueue *q, int starat_index);

/**
 * 存节点
 * @param q
 * @param pkt
 * @return
 */
int ff_packet_queue_put_private(struct PacketQueue *q, AVPacket *pkt);

#endif //FFPLAY2_FF_PACKET_QUEUE_H
