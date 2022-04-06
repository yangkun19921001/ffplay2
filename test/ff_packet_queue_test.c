//
// Created by 阳坤 on 2022/3/20.
// ffplay2 packet queue test
//
#include "../src/ff_packet_queue.h"

int main(int argc, char *args[]) {

    PacketQueue pctq, *pctqp = &pctq;
    AVPacket pkt;
    av_init_packet(&pkt);
    pkt.data = &pkt;

    int ret = ff_packet_queue_init(pctqp);
    pctqp->flush_pkt = &pkt;

    ff_packet_queue_start(pctqp);

    for (int i = 0; i < 100; ++i) {
        AVPacket pkt;
        av_init_packet(&pkt);
        ff_packet_queue_put(pctqp, &pkt);
        printf("queue size=%d \n",pctqp->nb_packets);
    }

    ff_packet_queue_flush(pctqp);
    ff_packet_queue_put(pctqp, pctqp->flush_pkt);
    for (int i = 0; i < 100; ++i) {
        AVPacket pkt;
        av_init_packet(&pkt);
        ff_packet_queue_put(pctqp, &pkt);
        printf("queue size=%d \n",pctqp->nb_packets);
    }
    ff_packet_queue_destroy(pctqp);
    printf("queue size=%d \n",pctqp->nb_packets);
    return 1;
}

