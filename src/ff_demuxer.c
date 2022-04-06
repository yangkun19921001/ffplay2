//
// Created by 阳坤 on 2022/3/20.
//

#include "ff_demuxer.h"

int ff_demuxer_init(struct FFplayer2 *fp) {
    VideoState *is = fp->is;
    AVFormatContext *ic = NULL;
    int err, i, ret = 0;
    if (fp->ffOptions->isDebug) {
        av_log_set_flags(AV_LOG_SKIP_REPEATED);
//    parse_loglevel(argc, argv, options);
        av_log_set_level(AV_LOG_ERROR);
    }
#if CONFIG_AVDEVICE
    avdevice_register_all();
#endif
    avformat_network_init();

    is->st_index[AVMEDIA_TYPE_NB];
    memset(is->st_index, -1, sizeof(is->st_index));
    int scan_all_pmts_set = 0;
    struct FFOptions *ops = fp->ffOptions;
    AVDictionaryEntry *t;
    // 初始化为-1,如果一直为-1说明没相应steam
    is->last_video_stream = is->video_stream = -1;
    is->last_audio_stream = is->audio_stream = -1;
    is->last_subtitle_stream = is->subtitle_stream = -1;
    is->eof = 0;    // =1是表明数据读取完毕
    // 1. 创建上下文结构体，这个结构体是最上层的结构体，表示输入上下文
    ic = avformat_alloc_context();
    if (!ic) {
        av_log(NULL, AV_LOG_FATAL, "Could not allocate context.\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }


    ic->interrupt_callback.opaque = is;

    //特定选项处理
    if (!av_dict_get(ops->format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE)) {
        av_dict_set(&ops->format_opts, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);
        scan_all_pmts_set = 1;
    }

    /* 3.打开文件，主要是探测协议类型，如果是网络文件则创建网络链接等 */
    err = avformat_open_input(&ic, is->filename, is->iformat, &ops->format_opts);
    if (err < 0) {
        print_error(is->filename, err);
        ret = -1;
        goto fail;
    }

    if (scan_all_pmts_set)
        av_dict_set(&ops->format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE);

    if ((t = av_dict_get(ops->format_opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
        av_log(NULL, AV_LOG_ERROR, "Option %s not found.\n", t->key);
        ret = AVERROR_OPTION_NOT_FOUND;
        goto fail;
    }

    is->ic = ic;    // videoState的ic指向分配的ic


    if (ops->genpts)
        ic->flags |= AVFMT_FLAG_GENPTS;

    av_format_inject_global_side_data(ic);


    if (ops->find_stream_info) {
        AVDictionary **opts = setup_find_stream_info_opts(ic, ops->codec_opts);
        int orig_nb_streams = ic->nb_streams;

        /*
         * 4.探测媒体类型，可得到当前文件的封装格式，音视频编码参数等信息
         * 调用该函数后得多的参数信息会比只调用avformat_open_input更为详细，
         * 其本质上是去做了decdoe packet获取信息的工作
         * codecpar, filled by libavformat on stream creation or
         * in avformat_find_stream_info()
         */
        err = avformat_find_stream_info(ic, opts);

        for (i = 0; i < orig_nb_streams; i++)
            av_dict_free(&opts[i]);
        av_freep(&opts);

        if (err < 0) {
            av_log(NULL, AV_LOG_WARNING,
                   "%s: could not find codec parameters\n", is->filename);
            ret = -1;
            goto fail;
        }
    }
    if (ic->pb)
        ic->pb->eof_reached = 0; // FIXME hack, ffplay maybe should not use avio_feof() to test for the end

    if (ops->seek_by_bytes < 0) {
        int flag = ic->iformat->flags & AVFMT_TS_DISCONT; //
        int cmp = strcmp("ogg", ic->iformat->name);
        ops->seek_by_bytes = !!(flag) && cmp;
    }

    is->max_frame_duration = (ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;
    if (!ops->window_title && (t = av_dict_get(ic->metadata, "title", NULL, 0)))
        ops->window_title = av_asprintf("%s - %s", t->value, ops->input_filename ? is->filename : ops->input_filename);


    /* if seeking requested, we execute it */
    /* 5. 检测是否指定播放起始时间 */
    if (ops->start_time != AV_NOPTS_VALUE) {
        int64_t timestamp;

        timestamp = ops->start_time;
        /* add the stream start time */
        if (ic->start_time != AV_NOPTS_VALUE)
            timestamp += ic->start_time;
        // seek的指定的位置开始播放
        ret = avformat_seek_file(ic, -1, INT64_MIN, timestamp, INT64_MAX, 0);
        if (ret < 0) {
            av_log(NULL, AV_LOG_WARNING, "%s: could not seek to position %0.3f\n",
                   is->filename, (double) timestamp / AV_TIME_BASE);
        }
    }


    /* 是否为实时流媒体 */
    is->realtime = is_realtime(ic);

    if (ops->show_status)
        av_dump_format(ic, 0, is->filename, 0);

    // 6. 查找AVStream
    // 6.1 根据用户指定来查找流,
    for (i = 0; i < ic->nb_streams; i++) {
        AVStream *st = ic->streams[i];
        enum AVMediaType type = st->codecpar->codec_type;
        //丢弃所有的帧
        //https://mp.weixin.qq.com/s/qPBSe0itF40ek1laIZRQnw
        st->discard = AVDISCARD_ALL;
        if (type >= 0 && ops->wanted_stream_spec[type] && is->st_index[type] == -1)
            if (avformat_match_stream_specifier(ic, st, ops->wanted_stream_spec[type]) > 0) {
                is->st_index[type] = i;
            }

    }
    for (i = 0; i < AVMEDIA_TYPE_NB; i++) {
        if (ops->wanted_stream_spec[i] && is->st_index[i] == -1) {
            av_log(NULL, AV_LOG_ERROR, "Stream specifier %s does not match any %s stream\n",
                   ops->wanted_stream_spec[i], av_get_media_type_string(i));
            is->st_index[i] = -1;
        }
    }

    // 6.2 利用av_find_best_stream选择流，
    if (!ops->video_disable)
        is->st_index[AVMEDIA_TYPE_VIDEO] =
                av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO,
                                    is->st_index[AVMEDIA_TYPE_VIDEO], -1, NULL, 0);
    if (!ops->audio_disable)
        is->st_index[AVMEDIA_TYPE_AUDIO] =
                av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO,
                                    is->st_index[AVMEDIA_TYPE_AUDIO],
                                    is->st_index[AVMEDIA_TYPE_VIDEO],
                                    NULL, 0);


    if (!ops->video_disable && !ops->subtitle_disable)
        is->st_index[AVMEDIA_TYPE_SUBTITLE] =
                av_find_best_stream(ic, AVMEDIA_TYPE_SUBTITLE,
                                    is->st_index[AVMEDIA_TYPE_SUBTITLE],
                                    (is->st_index[AVMEDIA_TYPE_AUDIO] >= 0 ?
                                     is->st_index[AVMEDIA_TYPE_AUDIO] :
                                     is->st_index[AVMEDIA_TYPE_VIDEO]),
                                    NULL, 0);

    is->show_mode = ops->show_mode;

    //创建解封装读取帧唤醒条件变量
    if (!(is->continue_read_thread = SDL_CreateCond())) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
        goto fail;
    }


    av_init_packet(&ops->flush_pkt);                // 初始化flush_packet
    ops->flush_pkt.data = (uint8_t *) &ops->flush_pkt; // 初始化为数据指向自己本身

    fail:
    if (ic && !is->ic)
        ff_demuxer_destory(fp);
    return ret;
}

/* pause or resume the video */
static inline void stream_toggle_pause(VideoState *is) {
    // 如果当前是暂停 -> 恢复播放
    // 正常播放 -> 暂停
    if (is->paused) {// 当前是暂停，那这个时候进来这个函数就是要恢复播放
        /* 恢复暂停状态时也需要恢复时钟，需要更新vidclk */
        // 加上 暂停->恢复 经过的时间
        is->frame_timer += av_gettime_relative() / 1000000.0 - is->vidclk.last_updated;
        if (is->read_pause_return != AVERROR(ENOSYS)) {
            is->vidclk.paused = 0;
        }
        // 设置时钟的意义，暂停状态下读取的是单纯pts
        // 重新矫正video时钟
        ff_set_clock(&is->vidclk, ff_get_clock(&is->vidclk), is->vidclk.serial);
    }
    ff_set_clock(&is->extclk, ff_get_clock(&is->extclk), is->extclk.serial);
    // 切换 pause/resume 两种状态
    is->paused = is->audclk.paused = is->vidclk.paused = is->extclk.paused = !is->paused;
    printf("is->step = %d; stream_toggle_pause\n", is->step);
}


static inline void step_to_next_frame(FFplayer2 *ffp) {
    VideoState *is = ffp->is;
    is->step = 1;
    /* if the stream is paused unpause it, then step */
    if (is->paused)
        stream_toggle_pause(ffp);
}

static int stream_has_enough_packets(AVStream *st, int stream_id, PacketQueue *queue) {
    return stream_id < 0 || // 没有该流
           queue->abort_request || // 请求退出
           (st->disposition & AV_DISPOSITION_ATTACHED_PIC) || // 是ATTACHED_PIC
           queue->nb_packets > MIN_FRAMES // packet数>25
           && (!queue->duration ||     // 满足PacketQueue总时长为0
               av_q2d(st->time_base) * queue->duration > 1.0); //或总时长超过1s
}


static inline int ff_demuxer_read_thread(void *arg) {
    av_log(NULL, AV_LOG_DEBUG, "ff_demuxer_read_thread() in\n");
    FFplayer2 *ff = arg;
    VideoState *is = ff->is;
    struct FFOptions *ops = ff->ffOptions;
    AVPacket pkt1, *pkt = &pkt1;
    int ret;
    int64_t pkt_ts;
    int pkt_in_play_range = 0;
    int64_t stream_start_time;
    SDL_mutex *wait_mutex = SDL_CreateMutex();
    // 一、准备流程
    if (!wait_mutex) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    while (1) {
        // 1 检测是否退出
        if (is->abort_request)
            break;

        // 2 检测是否暂停/继续
        if (is->paused != is->last_paused) {
            is->last_paused = is->paused;
            if (is->paused)
                is->read_pause_return = av_read_pause(is->ic); // 网络流的时候有用
            else
                av_read_play(is->ic);
        }
#if CONFIG_RTSP_DEMUXER || CONFIG_MMSH_PROTOCOL
        if (is->paused &&
                (!strcmp(ic->iformat->name, "rtsp") ||
                 (ic->pb && !strncmp(input_filename, "mmsh:", 5)))) {
            /* wait 10 ms to avoid trying to get another packet */
            /* XXX: horrible */
            // 等待10ms，避免立马尝试下一个Packet
            SDL_Delay(10);
            continue;
        }
#endif
        //  3 检测是否seek
        if (is->seek_req) { // 是否有seek请求
            int64_t seek_target = is->seek_pos; // 目标位置
            int64_t seek_min = is->seek_rel > 0 ? seek_target - is->seek_rel + 2 : INT64_MIN;
            int64_t seek_max = is->seek_rel < 0 ? seek_target - is->seek_rel - 2 : INT64_MAX;
            // 前进seek seek_rel>0
            //seek_min    = seek_target - is->seek_rel + 2;
            //seek_max    = INT64_MAX;
            // 后退seek seek_rel<0
            //seek_min = INT64_MIN;
            //seek_max = seek_target + |seek_rel| -2;
            //seek_rel =0  鼠标直接seek
            //seek_min = INT64_MIN;
            //seek_max = INT64_MAX;

            // FIXME the +-2 is due to rounding being not done in the correct direction in generation
            //      of the seek_pos/seek_rel variables
            // 修复由于四舍五入，没有再seek_pos/seek_rel变量的正确方向上进行
            int ret = avformat_seek_file(is->ic, -1, seek_min, seek_target, seek_max, is->seek_flags);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR,
                       "%s: error while seeking\n", is->ic->url);
            } else {
                /* seek的时候，要把原先的数据情况，并重启解码器，put flush_pkt的目的是告知解码线程需要
                 * reset decoder
                 */
                if (is->audio_stream >= 0) { // 如果有音频流
                    ff_packet_queue_flush(&is->audioq);    // 清空packet队列数据
                    // 放入flush pkt, 用来开起新的一个播放序列, 解码器读取到flush_pkt也清空解码器
                    ff_packet_queue_put(&is->audioq, &ops->flush_pkt);
                }
                if (is->subtitle_stream >= 0) { // 如果有字幕流
                    ff_packet_queue_flush(&is->subtitleq); // 和上同理
                    ff_packet_queue_put(&is->subtitleq, &ops->flush_pkt);
                }
                if (is->video_stream >= 0) {    // 如果有视频流
                    ff_packet_queue_flush(&is->videoq);    // 和上同理
                    ff_packet_queue_put(&is->videoq, &ops->flush_pkt);
                }

                if (is->seek_flags & AVSEEK_FLAG_BYTE) {
                    ff_set_clock(&is->extclk, NAN, 0);
                } else {
                    ff_set_clock(&is->extclk, seek_target / (double) AV_TIME_BASE, 0);
                }
            }
            is->seek_req = 0;
            is->queue_attachments_req = 1;
            is->eof = 0;        // 细节
            if (is->paused)
                step_to_next_frame(ff); // 播放seek后的第一帧
        }

        // 4 检测video是否为attached_pic
        if (is->queue_attachments_req) {
            // attached_pic 附带的图片。比如说一些MP3，AAC音频文件附带的专辑封面，所以需要注意的是音频文件不一定只存在音频流本身
            if (is->video_st && is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC) {
                AVPacket copy = {0};
                if ((ret = av_packet_ref(&copy, &is->video_st->attached_pic)) < 0)
                    goto fail;
                ff_packet_queue_put(&is->videoq, &copy);
                ff_packet_queue_put_nullpacket(&is->videoq, is->video_stream);
            }
            is->queue_attachments_req = 0;
        }

        // 5 检测队列是否已经有足够数据
        /* 缓存队列有足够的包，不需要继续读取数据 */
        if (ops->infinite_buffer < 1 &&
            (is->audioq.size + is->videoq.size + is->subtitleq.size > MAX_QUEUE_SIZE
             || (stream_has_enough_packets(is->audio_st, is->audio_stream, &is->audioq) &&
                 stream_has_enough_packets(is->video_st, is->video_stream, &is->videoq) &&
                 stream_has_enough_packets(is->subtitle_st, is->subtitle_stream, &is->subtitleq)))) {
            /* wait 10 ms */
            SDL_LockMutex(wait_mutex);
            // 如果没有唤醒则超时10ms退出，比如在seek操作时这里会被唤醒
            SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
            SDL_UnlockMutex(wait_mutex);
            continue;
        }

        // 6 检测码流是否已经播放结束
        if (!is->paused // 非暂停
            && // 这里的执行是因为码流读取完毕后 插入空包所致
            (!is->audio_st // 没有音频流
             || (is->auddec.finished == is->audioq.serial // 或者音频播放完毕
                 && ff_frame_queue_nb_remaining(&is->sampq) == 0))
            && (!is->video_st // 没有视频流
                || (is->viddec.finished == is->videoq.serial // 或者视频播放完毕
                    && ff_frame_queue_nb_remaining(&is->pictq) == 0))) {
            if (ops->loop != 1           // a 是否循环播放
                && (!ops->loop || --ops->loop)) {
                // stream_seek不是ffmpeg的函数，是ffplay封装的，每次seek的时候会调用
                ff_demuxer_stream_seek(is, ops->start_time != AV_NOPTS_VALUE ? ops->start_time : 0, 0, 0);
            } else if (ops->autoexit) {  // b 是否自动退出
                ret = AVERROR_EOF;
                goto fail;
            }
        }


        // 7.读取媒体数据，得到的是音视频分离后、解码前的数据
        ret = av_read_frame(is->ic, pkt); // 调用不会释放pkt的数据，需要我们自己去释放packet的数据

        if (pkt->stream_index == is->st_index[AVMEDIA_TYPE_VIDEO]) {
//            printf("video>>>>>>>>>>>>>>>>\n");
        }
        // 8 检测数据是否读取完毕
        if (ret < 0) {
            if ((ret == AVERROR_EOF || avio_feof(is->ic->pb))
                && !is->eof) {
                // 插入空包说明码流数据读取完毕了，之前讲解码的时候说过刷空包是为了从解码器把所有帧都读出来
                if (is->video_stream >= 0)
                    ff_packet_queue_put_nullpacket(&is->videoq, is->video_stream);
                if (is->audio_stream >= 0)
                    ff_packet_queue_put_nullpacket(&is->audioq, is->audio_stream);
                if (is->subtitle_stream >= 0)
                    ff_packet_queue_put_nullpacket(&is->subtitleq, is->subtitle_stream);
                is->eof = 1;        // 文件读取完毕
            }
            if (is->ic->pb && is->ic->pb->error)
                break;
            SDL_LockMutex(wait_mutex);
            SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
            SDL_UnlockMutex(wait_mutex);
            continue;        // 继续循环
        } else {
            is->eof = 0;
        }

        // 9 检测是否在播放范围内
        /* check if packet is in play range specified by user, then queue, otherwise discard */
        stream_start_time = is->ic->streams[pkt->stream_index]->start_time; // 获取流的起始时间
        pkt_ts = pkt->pts == AV_NOPTS_VALUE ? pkt->dts : pkt->pts; // 获取packet的时间戳
        // 这里的duration是在命令行时用来指定播放长度
        pkt_in_play_range = ops->duration == AV_NOPTS_VALUE ||
                            (pkt_ts - (stream_start_time != AV_NOPTS_VALUE ? stream_start_time : 0)) *
                            av_q2d(is->ic->streams[pkt->stream_index]->time_base) -
                            (double) (ops->start_time != AV_NOPTS_VALUE ? ops->start_time : 0) / 1000000
                            <= ((double) ops->duration / 1000000);

        // 10 将音视频数据分别送入相应的queue中
        if (pkt->stream_index == is->audio_stream && pkt_in_play_range) {
            ff_packet_queue_put(&is->audioq, pkt);
        } else if (pkt->stream_index == is->video_stream && pkt_in_play_range
                   && !(is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
            ff_packet_queue_put(&is->videoq, pkt);
        } else if (pkt->stream_index == is->subtitle_stream && pkt_in_play_range) {
            ff_packet_queue_put(&is->subtitleq, pkt);
        } else {
            av_packet_unref(pkt);// // 不入队列则直接释放数据
        }

    }
    // 三 退出线程处理
    ret = 0;
    fail:
    if (ret != 0) {
        SDL_Event event;
        event.type = FF_QUIT_EVENT;
        event.user.data1 = is;
        SDL_PushEvent(&event);
    }
    SDL_DestroyMutex(wait_mutex);
    return ret;
}

int ff_demuxer_start(struct FFplayer2 *fp) {
    VideoState *is = fp->is;
    if (!is)return AVERROR(EOBJNULL);
    /* 创建读线程 */
    is->read_tid = SDL_CreateThread(ff_demuxer_read_thread, "read_thread", fp);
    if (!is->read_tid) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateThread(): %s\n", SDL_GetError());
        ff_demuxer_destory(is);
        return AVERROR(ECTHREAD);
    }
    return 0;
}

void ff_demuxer_destory(struct FFplayer2 *fp) {
    VideoState *is = fp->is;
    if (is->ic) {
        avformat_close_input(&is->ic);
    }
    if (is->filename) {
        free(is->filename);
        is->filename = NULL;
    }
    is->ic = NULL;
    avformat_network_deinit();
}


void ff_demuxer_stream_seek(struct FFplayer2 *fp, int64_t pos, int64_t rel, int seek_by_bytes) {
    VideoState *is = fp->is;
    if (!is->seek_req) {
        is->seek_pos = pos; // 按时间微秒，按字节 byte
        is->seek_rel = rel;
        is->seek_flags &= ~AVSEEK_FLAG_BYTE;        // 不按字节的方式去seek
        if (seek_by_bytes)
            is->seek_flags |= AVSEEK_FLAG_BYTE;     // 强制按字节的方式去seek
        is->seek_req = 1;       // 请求seek， 在read_thread线程seek成功才将其置为0
        SDL_CondSignal(is->continue_read_thread);
    }
}

/* pause or resume the video */
void ff_demuxer_stream_pause(struct FFplayer2 *ff) {
    VideoState *is = ff->is;
    // 如果当前是暂停 -> 恢复播放
    // 正常播放 -> 暂停
    if (is->paused) {// 当前是暂停，那这个时候进来这个函数就是要恢复播放
        /* 恢复暂停状态时也需要恢复时钟，需要更新vidclk */
        // 加上 暂停->恢复 经过的时间
        is->frame_timer += av_gettime_relative() / 1000000.0 - is->vidclk.last_updated;
        if (is->read_pause_return != AVERROR(ENOSYS)) {
            is->vidclk.paused = 0;
        }
        // 设置时钟的意义，暂停状态下读取的是单纯pts
        // 重新矫正video时钟
        ff_set_clock(&is->vidclk, ff_get_clock(&is->vidclk), is->vidclk.serial);
    }
    ff_set_clock(&is->extclk, ff_get_clock(&is->extclk), is->extclk.serial);
    // 切换 pause/resume 两种状态
    is->paused = is->audclk.paused = is->vidclk.paused = is->extclk.paused = !is->paused;
    printf("is->step = %d; stream_toggle_pause\n", is->step);
}
