#include "../src/ff_demuxer.h"

static inline void print_pts(const char *tag, AVPacket *packet, AVRational timebase) {
    int64_t pts = packet->pts * (1000 * av_q2d(timebase));
    printf("%s pts=%lld keyframe=%d\n", tag, pts,
           strcmp("video_decode", tag) == 0 ? packet->flags == AV_PKT_FLAG_KEY : 0);
}

static inline void *video_decode(void *p) {
    FFplayer2 *fFplayer2 = p;
    int serinl = -1;
    AVPacket avpkt;
    while (1) {
        int ret = ff_packet_queue_get(&fFplayer2->is->videoq, &avpkt, 1, &serinl);
        if (ret) {
            print_pts("video_decode", &avpkt, fFplayer2->is->ic->streams[avpkt.stream_index]->time_base);
            av_packet_unref(&avpkt);
        } else if (ret < 0) {
            fFplayer2->is->videoq.abort_request = 1;
            break;
        }
    }
}

static inline void *audio_decode(void *p) {
    FFplayer2 *fFplayer2 = p;
    int serinl = -1;
    AVPacket avpkt;
    while (1) {
        int ret = ff_packet_queue_get(&fFplayer2->is->audioq, &avpkt, 1, &serinl);
        if (ret) {
            print_pts("audio_decode", &avpkt, fFplayer2->is->ic->streams[avpkt.stream_index]->time_base);
            av_packet_unref(&avpkt);
        } else if (ret < 0) {
            fFplayer2->is->audioq.abort_request = 1;
            break;
        }
    }
}


static inline void *subtitle_decode(void *p) {
    FFplayer2 *fFplayer2 = p;
    int serinl = -1;
    AVPacket avpkt;
    while (1) {
        int ret = ff_packet_queue_get(&fFplayer2->is->subtitleq, &avpkt, 1, &serinl);
        if (avpkt.data) {
            break;
        }
        if (ret) {
            print_pts("subtitle_decode", &avpkt, fFplayer2->is->ic->streams[avpkt.stream_index]->time_base);
            av_packet_unref(&avpkt);
        } else if (ret < 0) {
            fFplayer2->is->subtitleq.abort_request = 1;
            break;
        }
    }


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

    int ret = ff_demuxer_init(fFplayer2);

    av_init_packet(&fFplayer2->ffOptions->flush_pkt);
    fFplayer2->ffOptions->flush_pkt.data = (uint8_t *) &fFplayer2->ffOptions->flush_pkt; // 初始化为数据指向自己本身


    if (ff_packet_queue_init(&fFplayer2->is->videoq) < 0 ||
        ff_packet_queue_init(&fFplayer2->is->audioq) < 0 ||
        ff_packet_queue_init(&fFplayer2->is->subtitleq) < 0)
        return 0;

    fFplayer2->is->videoq.flush_pkt = &fFplayer2->ffOptions->flush_pkt;
    fFplayer2->is->audioq.flush_pkt = &fFplayer2->ffOptions->flush_pkt;
    fFplayer2->is->subtitleq.flush_pkt = &fFplayer2->ffOptions->flush_pkt;


    if (fFplayer2->is->st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        fFplayer2->is->video_stream = fFplayer2->is->st_index[AVMEDIA_TYPE_VIDEO];
        if (fFplayer2->is->video_stream >= 0)
            fFplayer2->is->video_st = fFplayer2->is->ic->streams[fFplayer2->is->video_stream];
        fFplayer2->is->ic->streams[fFplayer2->is->video_stream]->discard = AVDISCARD_DEFAULT;
        ff_packet_queue_start(&fFplayer2->is->videoq);
        SDL_CreateThread(video_decode, "video_decode", fFplayer2);
    }

    if (fFplayer2->is->st_index[AVMEDIA_TYPE_AUDIO] >= 0) {
        fFplayer2->is->audio_stream = fFplayer2->is->st_index[AVMEDIA_TYPE_AUDIO];
        if (fFplayer2->is->audio_stream >= 0)
            fFplayer2->is->audio_st = fFplayer2->is->ic->streams[fFplayer2->is->audio_stream];
        fFplayer2->is->ic->streams[fFplayer2->is->audio_stream]->discard = AVDISCARD_DEFAULT;
        ff_packet_queue_start(&fFplayer2->is->audioq);
        SDL_CreateThread(audio_decode, "audio_decode", fFplayer2);
    }

    if (fFplayer2->is->st_index[AVMEDIA_TYPE_SUBTITLE] >= 0) {
        fFplayer2->is->subtitle_stream = fFplayer2->is->st_index[AVMEDIA_TYPE_SUBTITLE];
        if (fFplayer2->is->subtitle_stream >= 0)
            fFplayer2->is->subtitle_st = fFplayer2->is->ic->streams[fFplayer2->is->subtitle_stream];
        fFplayer2->is->ic->streams[fFplayer2->is->subtitle_stream]->discard = AVDISCARD_DEFAULT;
        ff_packet_queue_start(&fFplayer2->is->subtitleq);
        SDL_CreateThread(subtitle_decode, "subtitle_decode", fFplayer2);
    }
    ff_demuxer_start(fFplayer2);
    while (1) {
        if (fFplayer2->is->videoq.abort_request && fFplayer2->is->audioq.abort_request)
            break;
        av_usleep(1000000);
    }
    ff_demuxer_destory(fFplayer2);
    printf("read packet eof\n");
    return ret;
}
