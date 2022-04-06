//
// Created by 阳坤 on 2022/3/21.
//

#include "ff_decoder.h"

/**
 * @brief 这里是设置给ffmpeg内部，当ffmpeg内部当执行耗时操作时（一般是在执行while或者for循环的数据读取时）
 *          就会调用该函数
 * @param ctx
 * @return 若直接退出阻塞则返回1，等待读取则返回0
 */

static int decode_interrupt_cb(void *ctx) {
    static int64_t s_pre_time = 0;
    int64_t cur_time = av_gettime_relative() / 1000;
    //    printf("decode_interrupt_cb interval:%lldms\n", cur_time - s_pre_time);
    s_pre_time = cur_time;
    VideoState *is = (VideoState *) ctx;
    return is->abort_request;
}

static void
ff_decoder_parameters_init(Decoder *d, AVCodecContext *avctx, PacketQueue *queue, SDL_cond *empty_queue_cond) {
    memset(d, 0, sizeof(Decoder));
    d->avctx = avctx;   // 解码器上下文
    d->queue = queue;   // 绑定对应的packet queue
    d->empty_queue_cond = empty_queue_cond; // 绑定read_thread线程的continue_read_thread
    d->start_pts = AV_NOPTS_VALUE;      // 起始设置为无效
    d->pkt_serial = -1;                 // 起始设置为-1
}

static int queue_picture(FFplayer2 *ff, AVFrame *src_frame, double pts,
                         double duration, int64_t pos, int serial) {
    Frame *vp;
    VideoState *is = ff->is;

#if defined(DEBUG_SYNC)
    printf("frame_type=%c pts=%0.3f\n",
           av_get_picture_type_char(src_frame->pict_type), pts);
#endif
    if (!(vp = ff_frame_queue_peek_writable(&is->pictq))) // 检测队列是否有可写空间
        return -1;      // 请求退出则返回-1
    // 执行到这步说已经获取到了可写入的Frame
    vp->sar = src_frame->sample_aspect_ratio;
    vp->uploaded = 0;

    vp->width = src_frame->width;
    vp->height = src_frame->height;
    vp->format = src_frame->format;

    vp->pts = pts;
    vp->duration = duration;
    vp->pos = pos;
    vp->serial = serial;

    if (ff->video_renderer_context && ff->video_renderer_context->set_default_window_size)
        ff->video_renderer_context->set_default_window_size(vp->width, vp->height, ff->ffOptions->screen_width,
                                                            ff->ffOptions->screen_height,
                                                            &ff->ffOptions->default_width,
                                                            &ff->ffOptions->default_height, vp->sar);

    av_frame_move_ref(vp->frame, src_frame); // 将src中所有数据转移到dst中，并复位src。
    ff_frame_queue_push(&is->pictq);   // 更新写索引位置
    return 0;
}

static int decoder_decode_frame(Decoder *d, AVFrame *frame, AVSubtitle *sub, struct FFOptions *ops) {
    int ret = AVERROR(EAGAIN);

    for (;;) {
        AVPacket pkt;
        // 1. 流连续情况下获取解码后的帧
        if (d->queue->serial == d->pkt_serial) { // 1.1 先判断是否是同一播放序列的数据
            do {
                if (d->queue->abort_request)
                    return -1;  // 是否请求退出
                // 1.2. 获取解码帧
                switch (d->avctx->codec_type) {
                    case AVMEDIA_TYPE_VIDEO:
                        ret = avcodec_receive_frame(d->avctx, frame);
                        //printf("frame pts:%ld, dts:%ld\n", frame->pts, frame->pkt_dts);
                        if (ret >= 0) {
                            if (ops->decoder_reorder_pts == -1) {
                                frame->pts = frame->best_effort_timestamp;
                            } else if (!ops->decoder_reorder_pts) {
                                frame->pts = frame->pkt_dts;
                            }
                        }
                        break;
                    case AVMEDIA_TYPE_AUDIO:
                        ret = avcodec_receive_frame(d->avctx, frame);
                        if (ret >= 0) {
                            AVRational tb = (AVRational) {1, frame->sample_rate};    //
                            if (frame->pts != AV_NOPTS_VALUE) {
                                // 如果frame->pts正常则先将其从pkt_timebase转成{1, frame->sample_rate}
                                // pkt_timebase实质就是stream->time_base
                                frame->pts = av_rescale_q(frame->pts, d->avctx->pkt_timebase, tb);
                            } else if (d->next_pts != AV_NOPTS_VALUE) {
                                // 如果frame->pts不正常则使用上一帧更新的next_pts和next_pts_tb
                                // 转成{1, frame->sample_rate}
                                frame->pts = av_rescale_q(d->next_pts, d->next_pts_tb, tb);
                            }
                            if (frame->pts != AV_NOPTS_VALUE) {
                                // 根据当前帧的pts和nb_samples预估下一帧的pts
                                d->next_pts = frame->pts + frame->nb_samples;
                                d->next_pts_tb = tb; // 设置timebase
                            }
                        }
                        break;
                }

                // 1.3. 检查解码是否已经结束，解码结束返回0
                if (ret == AVERROR_EOF) {
                    d->finished = d->pkt_serial;
                    printf("avcodec_flush_buffers %s(%d)\n", __FUNCTION__, __LINE__);
                    avcodec_flush_buffers(d->avctx);
                    return 0;
                }
                // 1.4. 正常解码返回1
                if (ret >= 0)
                    return 1;
            } while (ret != AVERROR(EAGAIN));   // 1.5 没帧可读时ret返回EAGIN，需要继续送packet
        }

        // 2 获取一个packet，如果播放序列不一致(数据不连续)则过滤掉“过时”的packet
        do {
            // 2.1 如果没有数据可读则唤醒read_thread, 实际是continue_read_thread SDL_cond
            if (d->queue->nb_packets == 0)  // 没有数据可读
                SDL_CondSignal(d->empty_queue_cond);// 通知read_thread放入packet
            // 2.2 如果还有pending的packet则使用它
            if (d->packet_pending) {
                av_packet_move_ref(&pkt, &d->pkt);
                d->packet_pending = 0;
            } else {
                // 2.3 阻塞式读取packet
                if (ff_packet_queue_get(d->queue, &pkt, 1, &d->pkt_serial) < 0)
                    return -1;
            }
            if (d->queue->serial != d->pkt_serial) {
                // darren自己的代码
                printf("%s(%d) discontinue:queue->serial:%d,pkt_serial:%d\n",
                       __FUNCTION__, __LINE__, d->queue->serial, d->pkt_serial);
                av_packet_unref(&pkt); // fixed me? 释放要过滤的packet
            }
        } while (d->queue->serial != d->pkt_serial);// 如果不是同一播放序列(流不连续)则继续读取

        // 3 将packet送入解码器
        if (pkt.data == ops->flush_pkt.data) {//
            // when seeking or when switching to a different stream
            avcodec_flush_buffers(d->avctx); //清空里面的缓存帧
            d->finished = 0;        // 重置为0
            d->next_pts = d->start_pts;     // 主要用在了audio
            d->next_pts_tb = d->start_pts_tb;// 主要用在了audio
        } else {
            if (d->avctx->codec_type == AVMEDIA_TYPE_SUBTITLE) {
                int got_frame = 0;
                ret = avcodec_decode_subtitle2(d->avctx, sub, &got_frame, &pkt);
                if (ret < 0) {
                    ret = AVERROR(EAGAIN);
                } else {
                    if (got_frame && !pkt.data) {
                        d->packet_pending = 1;
                        av_packet_move_ref(&d->pkt, &pkt);
                    }
                    ret = got_frame ? 0 : (pkt.data ? AVERROR(EAGAIN) : AVERROR_EOF);
                }
            } else {
                if (avcodec_send_packet(d->avctx, &pkt) == AVERROR(EAGAIN)) {
                    av_log(d->avctx, AV_LOG_ERROR,
                           "Receive_frame and send_packet both returned EAGAIN, which is an API violation.\n");
                    d->packet_pending = 1;
                    av_packet_move_ref(&d->pkt, &pkt);
                }
            }
            av_packet_unref(&pkt);    // 一定要自己去释放音视频数据
        }
    }
}

/**
 * @brief 获取视频帧
 * @param is
 * @param frame 指向获取的视频帧
 * @return
 */
static int get_video_frame(FFplayer2 *ff, AVFrame *frame) {
    VideoState *is = ff->is;
    int got_picture;
    // 1. 获取解码后的视频帧
    if ((got_picture = decoder_decode_frame(&is->viddec, frame, NULL, ff->ffOptions)) < 0) {
        return -1; // 返回-1意味着要退出解码线程, 所以要分析decoder_decode_frame什么情况下返回-1
    }

    if (got_picture) {
        // 2. 分析获取到的该帧是否要drop掉, 该机制的目的是在放入帧队列前先drop掉过时的视频帧
        double dpts = NAN;

        if (frame->pts != AV_NOPTS_VALUE)
            dpts = av_q2d(is->video_st->time_base) * frame->pts;    //计算出秒为单位的pts

        frame->sample_aspect_ratio = av_guess_sample_aspect_ratio(is->ic, is->video_st, frame);

        if (ff->ffOptions->framedrop > 0 || // 允许drop帧
            (ff->ffOptions->framedrop && ff_get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER))//非视频同步模式
        {
            if (frame->pts != AV_NOPTS_VALUE) { // pts值有效
                double diff = dpts - ff_get_master_clock(is);
                if (!isnan(diff) &&     // 差值有效
                    fabs(diff) < AV_NOSYNC_THRESHOLD && // 差值在可同步范围呢
                    diff - is->frame_last_filter_delay < 0 && // 和过滤器有关系
                    is->viddec.pkt_serial == is->vidclk.serial && // 同一序列的包
                    is->videoq.nb_packets) { // packet队列至少有1帧数据
                    is->frame_drops_early++;
                    printf("%s(%d) diff:%lfs, drop frame, drops:%d\n",
                           __FUNCTION__, __LINE__, diff, is->frame_drops_early);
                    av_frame_unref(frame);
                    got_picture = 0;
                }
            }
        }
    }

    return got_picture;
}

static int audio_decode_thread(void *arg) {
    FFplayer2 *ff = arg;
    VideoState *is = ff->is;

    AVFrame *frame = av_frame_alloc();
    Frame *af;
#if CONFIG_AVFILTER
    int last_serial = -1;
    int64_t dec_channel_layout;
    int reconfigure;
#endif
    int got_frame = 0;
    AVRational tb;
    int ret = 0;
    if (!frame)
        return AVERROR(ENOMEM);

    do {
        if ((got_frame = decoder_decode_frame(&is->auddec, frame, NULL, ff->ffOptions)) < 0)
            goto the_end;

        if (got_frame) {
            tb = (AVRational) {1, frame->sample_rate};

#if CONFIG_AVFILTER
            dec_channel_layout = get_valid_channel_layout(frame->channel_layout, frame->channels);

            reconfigure =
                cmp_audio_fmts(is->audio_filter_src.fmt, is->audio_filter_src.channels,
                               frame->format, frame->channels)    ||
                is->audio_filter_src.channel_layout != dec_channel_layout ||
                is->audio_filter_src.freq           != frame->sample_rate ||
                is->auddec.pkt_serial               != last_serial;

            if (reconfigure) {
                char buf1[1024], buf2[1024];
                av_get_channel_layout_string(buf1, sizeof(buf1), -1, is->audio_filter_src.channel_layout);
                av_get_channel_layout_string(buf2, sizeof(buf2), -1, dec_channel_layout);
                av_log(NULL, AV_LOG_DEBUG,
                       "Audio frame changed from rate:%d ch:%d fmt:%s layout:%s serial:%d to rate:%d ch:%d fmt:%s layout:%s serial:%d\n",
                       is->audio_filter_src.freq, is->audio_filter_src.channels, av_get_sample_fmt_name(is->audio_filter_src.fmt), buf1, last_serial,
                       frame->sample_rate, frame->channels, av_get_sample_fmt_name(frame->format), buf2, is->auddec.pkt_serial);

                is->audio_filter_src.fmt            = frame->format;
                is->audio_filter_src.channels       = frame->channels;
                is->audio_filter_src.channel_layout = dec_channel_layout;
                is->audio_filter_src.freq           = frame->sample_rate;
                last_serial                         = is->auddec.pkt_serial;

                if ((ret = configure_audio_filters(is, afilters, 1)) < 0)
                    goto the_end;
            }

        if ((ret = av_buffersrc_add_frame(is->in_audio_filter, frame)) < 0)
            goto the_end;

        while ((ret = av_buffersink_get_frame_flags(is->out_audio_filter, frame, 0)) >= 0) {
            tb = av_buffersink_get_time_base(is->out_audio_filter);
#endif
            if (!(af = ff_frame_queue_peek_writable(&is->sampq)))
                goto the_end;

            af->pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
            af->pos = frame->pkt_pos;
            af->serial = is->auddec.pkt_serial;
            af->duration = av_q2d((AVRational) {frame->nb_samples, frame->sample_rate});
            av_log(NULL, AV_LOG_DEBUG, "audio_decode_thread pts=%f\n", af->pts);

            av_frame_move_ref(af->frame, frame);
            ff_frame_queue_push(&is->sampq);

#if CONFIG_AVFILTER
            if (is->audioq.serial != is->auddec.pkt_serial)
                break;
        }
        if (ret == AVERROR_EOF)
            is->auddec.finished = is->auddec.pkt_serial;
#endif
        }
    } while (ret >= 0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);
    the_end:
#if CONFIG_AVFILTER
    avfilter_graph_free(&is->agraph);
#endif
    av_frame_free(&frame);
    return ret;
}

static int video_decode_thread(void *arg) {
    FFplayer2 *ff = arg;
    VideoState *is = ff->is;

    AVFrame *frame = av_frame_alloc();  // 分配解码帧
    double pts;                 // pts
    double duration;            // 帧持续时间
    int ret;
    //1 获取stream timebase
    AVRational tb = is->video_st->time_base; // 获取stream timebase
    //2 获取帧率，以便计算每帧picture的duration
    AVRational frame_rate = av_guess_frame_rate(is->ic, is->video_st, NULL);
#if CONFIG_AVFILTER
    AVFilterGraph *graph = NULL;
    AVFilterContext *filt_out = NULL, *filt_in = NULL;
    int last_w = 0;
    int last_h = 0;
    enum AVPixelFormat last_format = -2;
    int last_serial = -1;
    int last_vfilter_idx = 0;

#endif
    if (!frame)
        return AVERROR(ENOMEM);

    for (;;) {  // 循环取出视频解码的帧数据
        // 3 获取解码后的视频帧
        ret = get_video_frame(ff, frame);
        if (ret < 0)
            goto the_end;   //解码结束, 什么时候会结束
        if (!ret) {
            av_log(NULL, AV_LOG_DEBUG, "video_decode_error code=%d \n", ret);
            //没有解码得到画面, 什么情况下会得不到解后的帧
            continue;
        }

#if CONFIG_AVFILTER
        if (   last_w != frame->width
               || last_h != frame->height
               || last_format != frame->format
               || last_serial != is->viddec.pkt_serial
               || last_vfilter_idx != is->vfilter_idx) {
            av_log(NULL, AV_LOG_DEBUG,
                   "Video frame changed from size:%dx%d format:%s serial:%d to size:%dx%d format:%s serial:%d\n",
                   last_w, last_h,
                   (const char *)av_x_if_null(av_get_pix_fmt_name(last_format), "none"), last_serial,
                   frame->width, frame->height,
                   (const char *)av_x_if_null(av_get_pix_fmt_name(frame->format), "none"), is->viddec.pkt_serial);
            avfilter_graph_free(&graph);
            graph = avfilter_graph_alloc();
            if (!graph) {
                ret = AVERROR(ENOMEM);
                goto the_end;
            }
            graph->nb_threads = filter_nbthreads;
            if ((ret = configure_video_filters(graph, is, vfilters_list ? vfilters_list[is->vfilter_idx] : NULL, frame)) < 0) {
                SDL_Event event;
                event.type = FF_QUIT_EVENT;
                event.user.data1 = is;
                SDL_PushEvent(&event);
                goto the_end;
            }
            filt_in  = is->in_video_filter;
            filt_out = is->out_video_filter;
            last_w = frame->width;
            last_h = frame->height;
            last_format = frame->format;
            last_serial = is->viddec.pkt_serial;
            last_vfilter_idx = is->vfilter_idx;
            frame_rate = av_buffersink_get_frame_rate(filt_out);
        }

        ret = av_buffersrc_add_frame(filt_in, frame);
        if (ret < 0)
            goto the_end;

        while (ret >= 0) {
            is->frame_last_returned_time = av_gettime_relative() / 1000000.0;

            ret = av_buffersink_get_frame_flags(filt_out, frame, 0);
            if (ret < 0) {
                if (ret == AVERROR_EOF)
                    is->viddec.finished = is->viddec.pkt_serial;
                ret = 0;
                break;
            }

            is->frame_last_filter_delay = av_gettime_relative() / 1000000.0 - is->frame_last_returned_time;
            if (fabs(is->frame_last_filter_delay) > AV_NOSYNC_THRESHOLD / 10.0)
                is->frame_last_filter_delay = 0;
            tb = av_buffersink_get_time_base(filt_out);
#endif
        // 4 计算帧持续时间和换算pts值为秒
        // 1/帧率 = duration 单位秒, 没有帧率时则设置为0, 有帧率帧计算出帧间隔
        duration = (frame_rate.num && frame_rate.den ? av_q2d((AVRational) {frame_rate.den, frame_rate.num}) : 0);
        // 根据AVStream timebase计算出pts值, 单位为秒
        pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
        av_log(NULL, AV_LOG_DEBUG, "video_decode_thread pts=%f keyframe=%d \n", pts,
               frame->pict_type == AV_PICTURE_TYPE_I);
//        printf("%s(%d) , video_decode_thread pts=%fms keyframe=%d  size=%d\n",
//               __FUNCTION__, __LINE__, pts*1000,frame->pict_type == AV_PICTURE_TYPE_I,is->pictq.size);
        // 5 将解码后的视频帧插入队列
        ret = queue_picture(ff, frame, pts, duration, frame->pkt_pos, is->viddec.pkt_serial);
//        printf("%s(%d) , video_decode_thread pts=%fms keyframe=%d >>>>>ret=%d size=%d +++++++++++\n",
//               __FUNCTION__, __LINE__, pts,frame->pict_type == AV_PICTURE_TYPE_I,ret,is->pictq.size);
        // 6 释放frame对应的数据
        av_frame_unref(frame);
#if CONFIG_AVFILTER
        if (is->videoq.serial != is->viddec.pkt_serial)
                break;
        }
#endif

        if (ret < 0) // 返回值小于0则退出线程
            goto the_end;
    }
    the_end:
#if CONFIG_AVFILTER
    avfilter_graph_free(&graph);
#endif
    av_frame_free(&frame);
    return 0;
}

static int subtitle_decode_thread(void *arg) {
    FFplayer2 *ff = arg;
    VideoState *is = ff->is;
    Frame *sp;
    int got_subtitle;
    double pts;

    for (;;) {
        if (!(sp = ff_frame_queue_peek_writable(&is->subpq)))
            return 0;

        if ((got_subtitle = decoder_decode_frame(&is->subdec, NULL, &sp->sub, ff->ffOptions)) < 0)
            break;

        pts = 0;

        if (got_subtitle && sp->sub.format == 0) {
            if (sp->sub.pts != AV_NOPTS_VALUE)
                pts = sp->sub.pts / (double) AV_TIME_BASE;
            sp->pts = pts;
            sp->serial = is->subdec.pkt_serial;
            sp->width = is->subdec.avctx->width;
            sp->height = is->subdec.avctx->height;
            sp->uploaded = 0;

            /* now we can update the picture count */
            ff_frame_queue_push(&is->subpq);
        } else if (got_subtitle) {
            avsubtitle_free(&sp->sub);
        }
    }
    return 0;
}

static void ff_decoder_free(struct Decoder *d) {
    av_packet_unref(&d->pkt);
    avcodec_free_context(&d->avctx);
    d->avctx = NULL;
}


int ff_decoder_init(struct FFplayer2 *fp2) {
    int ret = -1;
    if (!fp2 || !fp2->is)
        goto fail;
    VideoState *is = fp2->is;

    /* 1.设置中断回调函数，如果出错或者退出，就根据目前程序设置的状态选择继续check或者直接退出 */
    /* 当执行耗时操作时（一般是在执行while或者for循环的数据读取时），会调用interrupt_callback.callback
     * 回调函数中返回1则代表ffmpeg结束耗时操作退出当前函数的调用
     * 回调函数中返回0则代表ffmpeg内部继续执行耗时操作，直到完成既定的任务(比如读取到既定的数据包)
     */
    is->ic->interrupt_callback.callback = decode_interrupt_cb;


    /* open the streams */
    /* 2. 打开视频、音频解码器。在此会打开相应解码器，并创建相应的解码线程。 */
    if (is->st_index[AVMEDIA_TYPE_AUDIO] >= 0) {// 如果有音频流则打开音频流
        ret = ff_stream_component_open(fp2, is->st_index[AVMEDIA_TYPE_AUDIO]);
    }
    if (is->st_index[AVMEDIA_TYPE_VIDEO] >= 0) { // 如果有视频流则打开视频流
        ret = ff_stream_component_open(fp2, is->st_index[AVMEDIA_TYPE_VIDEO]);
    }
    if (is->show_mode == SHOW_MODE_NONE) {
        //选择怎么显示，如果视频打开成功，就显示视频画面，否则，显示音频对应的频谱图
        is->show_mode = ret >= 0 ? SHOW_MODE_VIDEO : SHOW_MODE_RDFT;
    }

    if (is->st_index[AVMEDIA_TYPE_SUBTITLE] >= 0) { // 如果有字幕流则打开字幕流
        ret = ff_stream_component_open(fp2, is->st_index[AVMEDIA_TYPE_SUBTITLE]);
    }

    if (is->video_stream < 0 && is->audio_stream < 0) {
        av_log(NULL, AV_LOG_FATAL, "Failed to open file '%s' or configure filtergraph\n",
               is->filename);
        ret = -1;
        goto fail;
    }

    if (fp2->ffOptions->infinite_buffer < 0 && is->realtime)
        fp2->ffOptions->infinite_buffer = 1;    // 如果是实时流
    ret = 0;
    fail:
    return ret;
}


int ff_decoder_start(Decoder *d, int (*fn)(void *), const char *thread_name, void *arg) {
    ff_packet_queue_start(d->queue);   // 启用对应的packet 队列
    d->decoder_tid = SDL_CreateThread(fn, thread_name, arg);    // 创建解码线程
    if (!d->decoder_tid) {
        av_log(NULL, AV_LOG_ERROR, "SDL_CreateThread(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    return 0;
}

void ff_decoder_abort(struct Decoder *d, struct FrameQueue *fq) {
    ff_packet_queue_abort(d->queue);   // 终止packet队列，packetQueue的abort_request被置为1
    ff_frame_queue_signal(fq);         // 唤醒Frame队列, 以便退出
    SDL_WaitThread(d->decoder_tid, NULL);   // 等待解码线程退出
    d->decoder_tid = NULL;          // 线程ID重置
    ff_packet_queue_flush(d->queue);   // 情况packet队列，并释放数据
}

int ff_stream_component_open(struct FFplayer2 *ff, int stream_index) {
    VideoState *is = ff->is;
    AVFormatContext *ic = is->ic;
    AVCodecContext *avctx;
    AVCodec *codec;
    const char *forced_codec_name = NULL;
    AVDictionary *opts = NULL;
    AVDictionaryEntry *t = NULL;
    int sample_rate, nb_channels;
    int64_t channel_layout;
    int ret = 0;
    int stream_lowres = ff->ffOptions->lowres;
    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return -1;
    /*  为解码器分配一个编解码器上下文结构体 */
    avctx = avcodec_alloc_context3(NULL);
    if (!avctx) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    /* 将码流中的编解码器信息拷贝到新分配的编解码器上下文结构体 */
    ret = avcodec_parameters_to_context(avctx, ic->streams[stream_index]->codecpar);
    if (ret < 0)
        goto fail;
    // 设置pkt_timebase
    avctx->pkt_timebase = ic->streams[stream_index]->time_base;

    /* 根据codec_id查找解码器 */
    codec = avcodec_find_decoder(avctx->codec_id);

    switch (avctx->codec_type) {
        case AVMEDIA_TYPE_AUDIO   :
            is->last_audio_stream = stream_index;
            forced_codec_name = ff->ffOptions->audio_codec_name;
            break;
        case AVMEDIA_TYPE_SUBTITLE:
            is->last_subtitle_stream = stream_index;
            forced_codec_name = ff->ffOptions->subtitle_codec_name;
            break;
        case AVMEDIA_TYPE_VIDEO   :
            is->last_video_stream = stream_index;
            forced_codec_name = ff->ffOptions->video_codec_name;
            break;
    }

//    通过编码器名称来找
    if (forced_codec_name)
        codec = avcodec_find_decoder_by_name(forced_codec_name);
    if (!codec) {
        if (forced_codec_name)
            av_log(NULL, AV_LOG_WARNING,
                   "No codec could be found with name '%s'\n", forced_codec_name);
        else
            av_log(NULL, AV_LOG_WARNING,
                   "No decoder could be found for codec %s\n", avcodec_get_name(avctx->codec_id));
        ret = AVERROR(EINVAL);
        goto fail;
    }
    avctx->codec_id = codec->id;
    if (stream_lowres > codec->max_lowres) {
        av_log(avctx, AV_LOG_WARNING, "The maximum value for lowres supported by the decoder is %d\n",
               codec->max_lowres);
        stream_lowres = codec->max_lowres;
    }
    avctx->lowres = stream_lowres;

    if (ff->ffOptions->fast)
        avctx->flags2 |= AV_CODEC_FLAG2_FAST;

    opts = filter_codec_opts(codec_opts, avctx->codec_id, ic, ic->streams[stream_index], codec);

//    配置解码线程
    if (!av_dict_get(opts, "threads", NULL, 0))
        av_dict_set(&opts, "threads", "auto", 0);
    if (stream_lowres)
        av_dict_set_int(&opts, "lowres", stream_lowres, 0);
    if (avctx->codec_type == AVMEDIA_TYPE_VIDEO || avctx->codec_type == AVMEDIA_TYPE_AUDIO)
        av_dict_set(&opts, "refcounted_frames", "1", 0);

    //打开编码器
    ret = avcodec_open2(avctx, codec, &opts);
    if (ret < 0)
        goto fail;
    if ((t = av_dict_get(opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
        av_log(NULL, AV_LOG_ERROR, "Option %s not found.\n", t->key);
        ret = AVERROR_OPTION_NOT_FOUND;
        goto fail;
    }
    is->eof = 0;
    ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;

    switch (avctx->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
#if CONFIG_AVFILTER
            {
                AVFilterContext *sink;

                is->audio_filter_src.freq           = avctx->sample_rate;
                is->audio_filter_src.channels       = avctx->channels;
                is->audio_filter_src.channel_layout = get_valid_channel_layout(avctx->channel_layout, avctx->channels);
                is->audio_filter_src.fmt            = avctx->sample_fmt;
                if ((ret = configure_audio_filters(is, afilters, 0)) < 0)
                    goto fail;
                sink = is->out_audio_filter;
                sample_rate    = av_buffersink_get_sample_rate(sink);
                nb_channels    = av_buffersink_get_channels(sink);
                channel_layout = av_buffersink_get_channel_layout(sink);
            }
#else
            //从avctx(即AVCodecContext)中获取音频格式参数
            sample_rate = avctx->sample_rate;
            nb_channels = avctx->channels;
            channel_layout = avctx->channel_layout;
#endif
            /* prepare audio output 准备音频输出*/
            //调用audio_open打开sdl音频输出，实际打开的设备参数保存在audio_tgt，返回值表示输出设备的缓冲区大小
            if (!ff->audio_renderer_context || !ff->audio_renderer_context->audio_open ||
                (ret = ff->audio_renderer_context->audio_open(ff)) < 0) {
                goto fail;
            }
            is->audio_hw_buf_size = ret;
            is->audio_src = is->audio_tgt;  //暂且将数据源参数等同于目标输出参数
            //初始化audio_buf相关参数
            is->audio_buf_size = 0;
            is->audio_buf_index = 0;

            /* init averaging filter 初始化averaging滤镜, 非audio master时使用 */
            is->audio_diff_avg_coef = exp(log(0.01) / AUDIO_DIFF_AVG_NB); //0.794  exp，高等数学里以自然常数e为底的指数函数
            is->audio_diff_avg_count = 0;
            /* 由于我们没有精确的音频数据填充FIFO,故只有在大于该阈值时才进行校正音频同步*/
            is->audio_diff_threshold = (double) (is->audio_hw_buf_size) / is->audio_tgt.bytes_per_sec;

            is->audio_stream = stream_index;    // 获取audio的stream索引
            is->audio_st = ic->streams[stream_index];  // 获取audio的stream指针
            // 初始化ffplay封装的音频解码器
            ff_decoder_parameters_init(&is->auddec, avctx, &is->audioq, is->continue_read_thread);
            if ((is->ic->iformat->flags & (AVFMT_NOBINSEARCH | AVFMT_NOGENSEARCH | AVFMT_NO_BYTE_SEEK)) &&
                !is->ic->iformat->read_seek) {
                is->auddec.start_pts = is->audio_st->start_time;
                is->auddec.start_pts_tb = is->audio_st->time_base;
            }
            is->auddec.queue->flush_pkt = &ff->ffOptions->flush_pkt;
            // 启动音频解码线程
            if ((ret = ff_decoder_start(&is->auddec, audio_decode_thread, "audio_decoder", ff)) < 0)
                goto out;
            if (ff->audio_renderer_context)
                SDL_PauseAudioDevice(ff->audio_renderer_context->ffDevice.audio_dev, 0);
            break;
        case AVMEDIA_TYPE_VIDEO:
            is->video_stream = stream_index;    // 获取video的stream索引
            is->video_st = ic->streams[stream_index];// 获取video的stream指针
            // 初始化ffplay封装的视频解码器
            ff_decoder_parameters_init(&is->viddec, avctx, &is->videoq, is->continue_read_thread);
            is->viddec.queue->flush_pkt = &ff->ffOptions->flush_pkt;
            // 启动视频频解码线程
            if ((ret = ff_decoder_start(&is->viddec, video_decode_thread, "video_decoder", ff)) < 0)
                goto out;
            is->queue_attachments_req = 1; // 使能请求mp3、aac等音频文件的封面
            break;
        case AVMEDIA_TYPE_SUBTITLE:
            is->subtitle_stream = stream_index;
            is->subtitle_st = ic->streams[stream_index];

            ff_decoder_parameters_init(&is->subdec, avctx, &is->subtitleq, is->continue_read_thread);
            is->subdec.queue->flush_pkt = &ff->ffOptions->flush_pkt;
            if ((ret = ff_decoder_start(&is->subdec, subtitle_decode_thread, "subtitle_decoder", ff)) < 0)
                goto out;
            break;
        default:
            break;
    }

    fail:
    out:
    av_dict_free(&opts);
    return ret;
}

void ff_stream_component_close(struct FFplayer2 *ff, int stream_index) {
    VideoState *is = ff->is;
    AVFormatContext *ic = is->ic;
    AVCodecParameters *codecpar;

    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return;
    codecpar = ic->streams[stream_index]->codecpar;

    switch (codecpar->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
            ff_decoder_abort(&is->auddec, &is->sampq);
            if (ff->audio_renderer_context)
                SDL_CloseAudioDevice(ff->audio_renderer_context->ffDevice.audio_dev);
            ff_decoder_free(&is->auddec);
            swr_free(&is->swr_ctx);
            av_freep(&is->audio_buf1);
            is->audio_buf1_size = 0;
            is->audio_buf = NULL;

            if (is->rdft) {
                av_rdft_end(is->rdft);
                av_freep(&is->rdft_data);
                is->rdft = NULL;
                is->rdft_bits = 0;
            }
            break;
        case AVMEDIA_TYPE_VIDEO:
            ff_decoder_abort(&is->viddec, &is->pictq);
            ff_decoder_free(&is->viddec);
            break;
        case AVMEDIA_TYPE_SUBTITLE:
            ff_decoder_abort(&is->subdec, &is->subpq);
            ff_decoder_free(&is->subdec);
            break;
        default:
            break;
    }

    ic->streams[stream_index]->discard = AVDISCARD_ALL;
    switch (codecpar->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
            is->audio_st = NULL;
            is->audio_stream = -1;
            break;
        case AVMEDIA_TYPE_VIDEO:
            is->video_st = NULL;
            is->video_stream = -1;
            break;
        case AVMEDIA_TYPE_SUBTITLE:
            is->subtitle_st = NULL;
            is->subtitle_stream = -1;
            break;
        default:
            break;
    }
}

void ff_decoder_destroy(struct FFplayer2 *fp2) {
    VideoState *is = fp2->is;
    /* close each stream */
    if (is->audio_stream >= 0)
        ff_stream_component_close(fp2, is->audio_stream);
    if (is->video_stream >= 0)
        ff_stream_component_close(fp2, is->video_stream);
    if (is->subtitle_stream >= 0)
        ff_stream_component_close(fp2, is->subtitle_stream);
}


