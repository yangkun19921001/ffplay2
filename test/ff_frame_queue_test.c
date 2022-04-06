
#include "../src/ff_frame_queue.h"


static int *get_frame(void *p) {

    struct FrameQueue *frameQueue = (struct FrameQueue *) p;
    while (1) {
        Frame *frame = ff_frame_queue_peek_readable(frameQueue);
        printf("get frame pts=%f \n", frame->pts);
        av_usleep(2000000);
        ff_frame_queue_next(frameQueue);
    }
}

//
// Created by 阳坤 on 2022/3/20.
// ffplay2 frame queue test
//
int main(int argc, char *args[]) {

    FrameQueue vfque;
    PacketQueue vpque;
    memset(&vpque, 0, sizeof(PacketQueue));
    ff_frame_queue_init(&vfque, &vpque, 5, 1);
    SDL_CreateThread(get_frame, "get_frame", &vfque);
    AVFrame *pic = av_frame_alloc();
    for (int i = 0; i < 100; ++i) {
        Frame *frame = ff_frame_queue_peek_writable(&vfque);
        frame->pts = i * 1.0;
        av_frame_move_ref(frame->frame, pic);
        ff_frame_queue_push(&vfque);
        printf("put frame pts=%f \n", frame->pts);
    }
    ff_frame_queue_destory(&vfque);
    return 1;
}

