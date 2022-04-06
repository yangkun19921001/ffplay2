#include "../src/ff_demuxer.h"
#include "../src/ff_decoder.h"
#include "../src/ff_video_sdl_renderer.h"

static inline void print_pts(const char *tag, AVPacket *packet, AVRational timebase) {
    int64_t pts = packet->pts * (1000 * av_q2d(timebase));
    printf("%s pts=%lld keyframe=%d\n", tag, pts,
           strcmp("video_decode", tag) == 0 ? packet->flags == AV_PKT_FLAG_KEY : 0);
}

static inline void *video_decode(void *p) {
    FFplayer2 *fFplayer2 = p;
    struct FrameQueue *frameQueue = &fFplayer2->is->pictq;
    while (1) {
        Frame *frame = ff_frame_queue_peek_readable(frameQueue);
        printf("get video frame pts=%f \n", frame->pts);
        ff_frame_queue_next(frameQueue);
    }
}

static inline void *audio_decode(void *p) {
    FFplayer2 *fFplayer2 = p;
    struct FrameQueue *frameQueue = &fFplayer2->is->sampq;
    while (1) {
        Frame *frame = ff_frame_queue_peek_readable(frameQueue);
        printf("get audio frame pts=%f \n", frame->pts);
        ff_frame_queue_next(frameQueue);
    }
}


static inline void *subtitle_decode(void *p) {
    FFplayer2 *fFplayer2 = p;
    struct FrameQueue *frameQueue = &fFplayer2->is->subpq;
    while (1) {
        Frame *frame = ff_frame_queue_peek_readable(frameQueue);
        printf("get subtitle frame pts=%f \n", frame->pts);
        ff_frame_queue_next(frameQueue);
    }
}


 int video_open_(FFplayer2 *is) {
    return 1024;
}

 int audio_open_(FFplayer2 *is) {
    return 1024;
}

 void set_default_window_size_(int width, int height, int screen_width, int screen_height, int *default_width,
                                    int *default_height, AVRational sar) {

}




//
//ffmpeg 源码分析：https://blog.csdn.net/leixiaohua1020/article/details/44220151
// Created by 阳坤 on 2022/3/20.
//
int main(int argc, char *args[]) {
    av_log_set_flags(AV_LOG_SKIP_REPEATED);
    av_log_set_level(AV_LOG_INFO);
#if CONFIG_AVDEVICE
    avdevice_register_all();
#endif
    avformat_network_init();
    const char *url = "http://rescdn.yishihui.com/longvideo/transcode/video/vpc/20200911/1253281k1Kyke1nwH5Ftq4vqa.mp4";
    FFplayer2 *fFplayer2 = malloc(sizeof(FFplayer2));
    memset(fFplayer2, 0, sizeof(FFplayer2));
    fFplayer2->ffOptions = (FFOptions *) malloc(sizeof(FFOptions));
    memset(fFplayer2->ffOptions, 0, sizeof(FFOptions));
    initFFOptions(fFplayer2->ffOptions);
    fFplayer2->is = (VideoState *) malloc(sizeof(VideoState));
    memset(fFplayer2->is, 0, sizeof(VideoState));
    fFplayer2->is->filename = av_strdup(url);

    AVRendererContext *arc = malloc(sizeof(AVRendererContext));
    AVRendererContext *vrc = malloc(sizeof(AVRendererContext));
    fFplayer2->audio_renderer_context = arc;
    fFplayer2->video_renderer_context = vrc;

    fFplayer2->audio_renderer_context->audio_open = audio_open_;
    fFplayer2->video_renderer_context->video_open = video_open_;
    fFplayer2->video_renderer_context->set_default_window_size = set_default_window_size_;
    int ret = ff_demuxer_init(fFplayer2);

    av_init_packet(&fFplayer2->ffOptions->flush_pkt);
    fFplayer2->ffOptions->flush_pkt.data = (uint8_t *) &fFplayer2->ffOptions->flush_pkt; // 初始化为数据指向自己本身

    /* 初始化帧队列 */
    if (ff_frame_queue_init(&fFplayer2->is->pictq, &fFplayer2->is->videoq, VIDEO_PICTURE_QUEUE_SIZE, 1) < 0)
        return 0;
    if (ff_frame_queue_init(&fFplayer2->is->subpq, &fFplayer2->is->subtitleq, SUBPICTURE_QUEUE_SIZE, 0) < 0)
        return 0;
    if (ff_frame_queue_init(&fFplayer2->is->sampq, &fFplayer2->is->audioq, SAMPLE_QUEUE_SIZE, 1) < 0)
        return 0;


    if (ff_packet_queue_init(&fFplayer2->is->videoq) < 0 ||
        ff_packet_queue_init(&fFplayer2->is->audioq) < 0 ||
        ff_packet_queue_init(&fFplayer2->is->subtitleq) < 0)
        return 0;

    /*
    * 初始化时钟
    * 时钟序列->queue_serial，实际上指向的是is->videoq.serial
    */
    ff_init_clock(&fFplayer2->is->vidclk, &fFplayer2->is->videoq.serial);
    ff_init_clock(&fFplayer2->is->audclk, &fFplayer2->is->audioq.serial);
    ff_init_clock(&fFplayer2->is->extclk, &fFplayer2->is->extclk.serial);
    fFplayer2->is->audio_clock_serial = -1;

    fFplayer2->is->videoq.flush_pkt = &fFplayer2->ffOptions->flush_pkt;
    fFplayer2->is->audioq.flush_pkt = &fFplayer2->ffOptions->flush_pkt;
    fFplayer2->is->subtitleq.flush_pkt = &fFplayer2->ffOptions->flush_pkt;

    ff_demuxer_start(fFplayer2);
    ff_decoder_init(fFplayer2);

    if (fFplayer2->is->st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        SDL_CreateThread(video_decode, "video_decode", fFplayer2);
    }

    if (fFplayer2->is->st_index[AVMEDIA_TYPE_AUDIO] >= 0) {
        SDL_CreateThread(audio_decode, "audio_decode", fFplayer2);
    }

    if (fFplayer2->is->st_index[AVMEDIA_TYPE_SUBTITLE] >= 0) {
        SDL_CreateThread(subtitle_decode, "subtitle_decode", fFplayer2);
    }

    ff_demuxer_stream_seek(fFplayer2,200*AV_TIME_BASE,20*AV_TIME_BASE,0);
    while (1) {
        if (fFplayer2->is->videoq.abort_request && fFplayer2->is->audioq.abort_request)
            break;
        av_usleep(1000000);
    }
    ff_demuxer_destory(fFplayer2);
    printf("read packet eof\n");
    return ret;
}
