//
// Created by 阳坤 on 2022/3/22.
//

#include "ff_sync_renderer.h"


static int ff_avrenderer_create_renderer_ctx(struct AVRendererContext **ctx) {
    *ctx = (struct AVRendererContext *) malloc(sizeof(AVRendererContext));
    if (!*ctx)
        return -1;
    memset(*ctx, 0, sizeof(AVRendererContext));
    memset(&(*ctx)->ffDevice, 0, sizeof(FFAVDevice));
    return 0;
}

static int ff_avrenderer_free_renderer_ctx(struct AVRendererContext **ctx) {
    if (*ctx) {
        free(*ctx);
        *ctx = NULL;
        return 0;
    }
    return -1;
}

/**
 * 查找渲染器
 * @return
 */
static int ff_avrenderer_config_renderer(struct AVRendererContext *ctx) {
    if (!ctx)return -1;
    switch (ctx->rendererId.rp) {
        case PC:
            ctx->rendererId.dy = SDL;
            return 0;
        case IOS:
            if (ctx->mediaType == AVMEDIA_TYPE_AUDIO) {
                ctx->rendererId.dy = SDL;
            } else {
                ctx->rendererId.dy = OPENGL_ES;
            }
            return 0;
        case ANDROID:
            if (ctx->mediaType == AVMEDIA_TYPE_AUDIO) {
                ctx->rendererId.dy = OPENSL_ES;
            } else {
                ctx->rendererId.dy = OPENGL_ES;
            }
            return 0;
        default:
            break;
    }
    return -1;
};


/**
 * 视频真正显示的地方
 * @param ff
 */
static void video_display(struct FFplayer2 *ffp2) {
    if (ffp2->video_renderer_context->video_renderer) {
        ffp2->video_renderer_context->video_renderer(ffp2);
    }
}


// 计算上一帧需要持续的duration，这里有校正算法
static double vp_duration(VideoState *is, Frame *vp, Frame *nextvp) {
    if (vp->serial == nextvp->serial) { // 同一播放序列，序列连续的情况下
        double duration = nextvp->pts - vp->pts;
        if (isnan(duration) // duration 数值异常
            || duration <= 0    // pts值没有递增时
            || duration > is->max_frame_duration    // 超过了最大帧范围
                ) {
            return vp->duration;     /* 异常时以帧时间为基准(1秒/帧率) */
        } else {
            return duration; //使用两帧pts差值计算duration，一般情况下也是走的这个分支
        }
    } else {        // 不同播放序列, 序列不连续则返回0
        return 0.0;
    }
}

/**
 * @brief 计算正在显示帧需要持续播放的时间。
 * @param delay 该参数实际传递的是当前显示帧和待播放帧的间隔。
 * @param is
 * @return 返回当前显示帧要持续播放的时间。为什么要调整返回的delay？为什么不支持使用相邻间隔帧时间？
 */
static double compute_target_delay(double delay, VideoState *is) {
    double sync_threshold, diff = 0;

    /* update delay to follow master synchronisation source */
    /* 如果发现当前主Clock源不是video，则计算当前视频时钟与主时钟的差值 */
    if (ff_get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER) {
        /* if video is slave, we try to correct big delays by
           duplicating or deleting a frame
           通过重复帧或者删除帧来纠正延迟*/
        diff = ff_get_clock(&is->vidclk) - ff_get_master_clock(is);
        av_log(NULL, AV_LOG_INFO, "video: clock=%0.3f master: clock=%0.3f \n",
               ff_get_clock(&is->vidclk), ff_get_master_clock(is));
        /* skip or repeat frame. We take into account the
           delay to compute the threshold. I still don't know
           if it is the best guess */
        sync_threshold = FFMAX(AV_SYNC_THRESHOLD_MIN,
                               FFMIN(AV_SYNC_THRESHOLD_MAX, delay));
        printf("video_clock=%f master_clock=%f delay=%f diff=%f  sync_threshold=%f \n",ff_get_clock(&is->vidclk),ff_get_master_clock(is),delay,diff,sync_threshold);
        if (!isnan(diff) && fabs(diff) < is->max_frame_duration) { // diff在最大帧duration内
            if (diff <= -sync_threshold) {      // 视频已经落后了
                delay = FFMAX(0, delay + diff); // 上一帧持续的时间往小的方向去调整
                printf("视频显示过慢 pts=%f \n",delay);
            } else if (diff >= sync_threshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD) {//视频过快
                //  delay = 0.2秒
                // diff  = 1秒
                // delay = 0.2 + 1 = 1.2
                // 视频超前
                //AV_SYNC_FRAMEDUP_THRESHOLD是0.1，此时如果delay>0.1, 如果2*delay时间就有点久
                delay = delay + diff; // 上一帧持续时间往大的方向去调整
                av_log(NULL, AV_LOG_ERROR, "video: delay=%0.3f A-V=%f\n",
                       delay, -diff);
                printf("视频显示过快 pts=%f \n",delay);
            } else if (diff >= sync_threshold) { //其实还是视频过快，差值已经大于 1 帧 pts 了
                // 上一帧持续时间往大的方向去调整
                // delay = 0.2 *2 = 0.4
                delay = 2 * delay; // 保持在 2 * AV_SYNC_FRAMEDUP_THRESHOLD内, 即是2*0.1 = 0.2秒内
//                delay = delay + diff; // 上一帧持续时间往大的方向去调整
            } else {
                // 音视频同步精度在 -sync_threshold ~ +sync_threshold
                // 其他条件就是 delay = delay; 维持原来的delay, 依靠frame_timer+duration和当前时间进行对比
            }
        }
    } else {
        // 如果是以video为同步，则直接返回last_duration
    }

    av_log(NULL, AV_LOG_TRACE, "video: delay=%0.3f A-V=%f\n",
           delay, -diff);

    return delay;
}


static void update_video_pts(VideoState *is, double pts, int64_t pos, int serial) {
    /* update current video pts */
    ff_set_clock(&is->vidclk, pts, serial);
    ff_sync_clock_to_slave(&is->extclk, &is->vidclk);
}


static void video_refresh(void *opaque, double *remaining_time) {
    FFplayer2 *ff = opaque;
    VideoState *is = ff->is;
    double time;

    Frame *sp, *sp2;

    if (!is->paused && ff_get_master_sync_type(is) == AV_SYNC_EXTERNAL_CLOCK && is->realtime)
        ff_check_external_clock_speed(is);


    if (!is->paused && ff_get_master_sync_type(is) == AV_SYNC_EXTERNAL_CLOCK && is->realtime)
        ff_check_external_clock_speed(is);

    if (!ff->ffOptions->display_disable && is->show_mode != SHOW_MODE_VIDEO && is->audio_st) {
        time = av_gettime_relative() / 1000000.0;
        if (is->force_refresh || is->last_vis_time + ff->ffOptions->rdftspeed < time) {
            video_display(ff);
            is->last_vis_time = time;
        }
        *remaining_time = FFMIN(*remaining_time, is->last_vis_time + ff->ffOptions->rdftspeed - time);
    }

    if (is->video_st) {
        retry:
        if (ff_frame_queue_nb_remaining(&is->pictq) == 0) {// 帧队列是否为空
            // nothing to do, no picture to display in the queue
            // 什么都不做，队列中没有图像可显示
            printf("not packet\n");
        } else {// 重点是音视频同步
            double last_duration, duration, delay;
            Frame *vp, *lastvp;

            /* dequeue the picture */
            // 从队列取出上一个Frame
            lastvp = ff_frame_queue_peek_last(&is->pictq);//读取上一帧
            vp = ff_frame_queue_peek(&is->pictq);  // 读取待显示帧
            // lastvp 上一帧(正在显示的帧)
            // vp 等待显示的帧
            if (vp->serial != is->videoq.serial) {
                // 如果不是最新的播放序列，则将其出队列，以尽快读取最新序列的帧
                ff_frame_queue_next(&is->pictq);
//                printf("%s(%d) , video_refresh size=%d --------------\n",
//                       __FUNCTION__, __LINE__, is->pictq.size);
                goto retry;
            }
            // lastvp和vp不是同一播放序列(一个seek会开始一个新播放序列)，将frame_timer更新为当前时间
            if (lastvp->serial != vp->serial) {
                // 新的播放序列重置当前时间
                is->frame_timer = av_gettime_relative() / 1000000.0;
            }

            // 暂停处理：不停播放上一帧图像
            if (is->paused) {
                goto display;
                printf("视频暂停is->paused");
            }
            /* compute nominal last_duration */
            //lastvp上一帧，vp当前帧 ，nextvp下一帧
            //last_duration 计算上一帧应显示的时长
            last_duration = vp_duration(is, lastvp, vp); // 上一帧播放时长：vp->pts - lastvp->pts

            // 经过compute_target_delay方法，计算出待显示帧vp需要等待的时间
            // 如果以video同步，则delay直接等于last_duration。
            // 如果以audio或外部时钟同步，则需要比对主时钟调整待显示帧vp要等待的时间。
            delay = compute_target_delay(last_duration, is); // 上一帧需要维持的时间
            time = av_gettime_relative() / 1000000.0;
            // is->frame_timer 实际上就是上一帧lastvp的播放时间,
            // is->frame_timer + delay 是待显示帧vp该播放的时间
            if (time < is->frame_timer + delay) { //判断是否继续显示上一帧
                // 当前系统时刻还未到达上一帧的结束时刻，那么还应该继续显示上一帧。
                // 计算出最小等待时间
                *remaining_time = FFMIN(is->frame_timer + delay - time, *remaining_time);
                goto display;
            }

            // 走到这一步，说明已经到了或过了该显示的时间，待显示帧vp的状态变更为当前要显示的帧

            is->frame_timer += delay;   // 更新当前帧播放的时间
            if (delay > 0 && time - is->frame_timer > AV_SYNC_THRESHOLD_MAX) {
                is->frame_timer = time; //如果和系统时间差距太大，就纠正为系统时间
            }
            SDL_LockMutex(is->pictq.mutex);
            if (!isnan(vp->pts)) {
                update_video_pts(is, vp->pts, vp->pos, vp->serial); // 更新video时钟
            }
            SDL_UnlockMutex(is->pictq.mutex);
            //丢帧逻辑
            if (ff_frame_queue_nb_remaining(&is->pictq) > 1) {//有nextvp才会检测是否该丢帧
                Frame *nextvp = ff_frame_queue_peek_next(&is->pictq);
                duration = vp_duration(is, vp, nextvp);
                if (!is->step        // 非逐帧模式才检测是否需要丢帧 is->step==1 为逐帧播放
                    && (ff->ffOptions->framedrop > 0 ||      // cpu解帧过慢
                        (ff->ffOptions->framedrop && ff_get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)) // 非视频同步方式
                    && time > is->frame_timer + duration // 确实落后了一帧数据
                        ) {
                    printf("%s(%d) dif:%lfs, drop frame\n", __FUNCTION__, __LINE__,
                           (is->frame_timer + duration) - time);
                    is->frame_drops_late++;             // 统计丢帧情况
                    ff_frame_queue_next(&is->pictq);       // 这里实现真正的丢帧
                    //(这里不能直接while丢帧，因为很可能audio clock重新对时了，这样delay值需要重新计算)
                    goto retry; //回到函数开始位置，继续重试
                }
            }


            ff_frame_queue_next(&is->pictq);   // 当前vp帧出队列
            is->force_refresh = 1;          /* 说明需要刷新视频帧 */

            if (is->step && !is->paused)
                ff_demuxer_stream_pause(ff);    // 逐帧的时候那继续进入暂停状态
        }
        display:
        /* display picture */
        if (!ff->ffOptions->display_disable && is->force_refresh && is->show_mode == SHOW_MODE_VIDEO &&
            is->pictq.rindex_shown)
            video_display(ff); // 重点是显示
    }
    is->force_refresh = 0;
    if (ff->ffOptions->show_status) {
        static int64_t last_time;
        int64_t cur_time;
        int aqsize, vqsize, sqsize;
        double av_diff;

        cur_time = av_gettime_relative();
        if (!last_time || (cur_time - last_time) >= 30000) {
            aqsize = 0;
            vqsize = 0;
            sqsize = 0;
            if (is->audio_st)
                aqsize = is->audioq.size;
            if (is->video_st)
                vqsize = is->videoq.size;
            if (is->subtitle_st)
                sqsize = is->subtitleq.size;
            av_diff = 0;
            if (is->audio_st && is->video_st)
                av_diff = ff_get_clock(&is->audclk) - ff_get_clock(&is->vidclk);
            else if (is->video_st)
                av_diff = ff_get_master_clock(is) - ff_get_clock(&is->vidclk);
            else if (is->audio_st)
                av_diff = ff_get_master_clock(is) - ff_get_clock(&is->audclk);
            av_log(NULL, AV_LOG_INFO,
                   "%7.2f %s:%7.3f fd=%4d aq=%5dKB vq=%5dKB sq=%5dB f=%"PRId64"/%"PRId64"   \r",
                   ff_get_master_clock(is),
                   (is->audio_st && is->video_st) ? "A-V" : (is->video_st ? "M-V" : (is->audio_st ? "M-A" : "   ")),
                   av_diff,
                   is->frame_drops_early + is->frame_drops_late,
                   aqsize / 1024,
                   vqsize / 1024,
                   sqsize,
                   is->video_st ? is->viddec.avctx->pts_correction_num_faulty_dts : 0,
                   is->video_st ? is->viddec.avctx->pts_correction_num_faulty_pts : 0);
            fflush(stdout);
            last_time = cur_time;
        }
    }
}

void ff_refresh_loop_wait_event(FFplayer2 *ffp2, SDL_Event *event) {
    VideoState *is = ffp2->is;
    double remaining_time = 0.0; /* 休眠等待，remaining_time的计算在video_refresh中 */
    /* 调用SDL_PeepEvents前先调用SDL_PumpEvents，将输入设备的事件抽到事件队列中 */
    SDL_PumpEvents();
    /*
     * SDL_PeepEvents check是否事件，比如鼠标移入显示区等
     * 从事件队列中拿一个事件，放到event中，如果没有事件，则进入循环中
     * SDL_PeekEvents用于读取事件，在调用该函数之前，必须调用SDL_PumpEvents搜集键盘等事件
     */
    while (!SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT)) {
        if (!ffp2->ffOptions->cursor_hidden &&
            av_gettime_relative() - ffp2->ffOptions->cursor_last_shown > CURSOR_HIDE_DELAY) {
            SDL_ShowCursor(0);
            ffp2->ffOptions->cursor_hidden = 1;
        }
        /*
         * remaining_time就是用来进行音视频同步的。
         * 在video_refresh函数中，根据当前帧显示时刻(display time)和实际时刻(actual time)
         * 计算需要sleep的时间，保证帧按时显示
         */
        if (remaining_time > 0.0) {
//            printf("remaining_time =%lf \n",remaining_time);
            //sleep控制画面输出的时机
            av_usleep((int64_t) (remaining_time * 1000000.0)); // remaining_time <= REFRESH_RATE
        }
        remaining_time = REFRESH_RATE;
        if (is->show_mode != SHOW_MODE_NONE && // 显示模式不等于SHOW_MODE_NONE
            (!is->paused  // 非暂停状态
             || is->force_refresh) // 强制刷新状态
                ) {
            video_refresh(ffp2, &remaining_time);
        }
        /* 从输入设备中搜集事件，推动这些事件进入事件队列，更新事件队列的状态，
         * 不过它还有一个作用是进行视频子系统的设备状态更新，如果不调用这个函数，
         * 所显示的视频会在大约10秒后丢失色彩。没有调用SDL_PumpEvents，将不会
         * 有任何的输入设备事件进入队列，这种情况下，SDL就无法响应任何的键盘等硬件输入。
        */
        SDL_PumpEvents();
    }
}

int ff_sync_renderer_init(struct FFplayer2 *ffp2) {
    VideoState *is = ffp2->is;
    FFOptions *ops = ffp2->ffOptions;
    is->ytop = 0;
    is->xleft = 0;
    int ret = -1;
    AVRendererContext *arc = NULL;
    ff_avrenderer_create_renderer_ctx(&arc);
    if (arc) {
        arc->mediaType = AVMEDIA_TYPE_AUDIO;
        arc->rendererId.dy = (enum AVDisplayType) PC;
        arc->rendererId.rp = (enum AVRendererPlatform) SDL;
        ffp2->audio_renderer_context = arc;
        ffp2->audio_renderer_context->audio_open = audio_open;
    }
    AVRendererContext *vrc = NULL;
    ff_avrenderer_create_renderer_ctx(&vrc);
    if (vrc) {
        vrc->mediaType = AVMEDIA_TYPE_VIDEO;
        vrc->rendererId.dy = (enum AVDisplayType) PC;
        vrc->rendererId.rp = (enum AVRendererPlatform) SDL;
        ffp2->video_renderer_context = vrc;
        ffp2->video_renderer_context->set_default_window_size = set_default_window_size;
        ffp2->video_renderer_context->video_open = video_open;
        ffp2->video_renderer_context->video_renderer = video_renderer;
        create_sdl_window(ffp2,&ffp2->video_renderer_context->ffDevice);
    }
    /*
     * 初始化时钟
     * 时钟序列->queue_serial，实际上指向的是is->videoq.serial
     */
    ff_init_clock(&is->vidclk, &is->videoq.serial);
    ff_init_clock(&is->audclk, &is->audioq.serial);
    ff_init_clock(&is->extclk, &is->extclk.serial);
    is->audio_clock_serial = -1;

    //初始化音频音量
    if (ops->startup_volume < 0)
        av_log(NULL, AV_LOG_WARNING, "-volume=%d < 0, setting to 0\n", ops->startup_volume);
    if (ops->startup_volume > 100)
        av_log(NULL, AV_LOG_WARNING, "-volume=%d > 100, setting to 100\n", ops->startup_volume);


    ops->startup_volume = av_clip(ops->startup_volume, 0, 100);
    ops->startup_volume = av_clip(SDL_MIX_MAXVOLUME * ops->startup_volume / 100, 0, SDL_MIX_MAXVOLUME);
    is->audio_volume = ops->startup_volume;
    is->muted = 0;
    //同步类型，默认以音频为基准
    is->av_sync_type = ops->av_sync_type;


    //        渲染初始化
    int flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER;
    if (SDL_Init(flags)) {
        av_log(NULL, AV_LOG_FATAL, "Could not initialize SDL - %s\n", SDL_GetError());
        av_log(NULL, AV_LOG_FATAL, "(Did you set the DISPLAY variable?)\n");
        exit(1);
    }
    ret = 0;
    fail:
    return ret;
}

int ff_sync_renderer_start(struct FFplayer2 *player) {
    if (player->is->st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        AVStream *st = player->is->ic->streams[player->is->st_index[AVMEDIA_TYPE_VIDEO]];
        AVCodecParameters *codecpar = st->codecpar;
        //根据流和帧宽高比猜测视频帧的像素宽高比（像素的宽高比，注意不是图像的）
        AVRational sar = av_guess_sample_aspect_ratio(player->is->ic, st, NULL);
        if (codecpar->width && player->video_renderer_context &&
            player->video_renderer_context->set_default_window_size) {
            //设置显示窗口的大小和宽高比
            player->video_renderer_context->set_default_window_size(codecpar->width, codecpar->height,
                                                                    player->ffOptions->screen_width,
                                                                    player->ffOptions->screen_height,
                                                                    &player->ffOptions->default_width,
                                                                    &player->ffOptions->default_height, sar);
        }
    }
    return 0;
}

int ff_sync_renderer_destory(struct FFplayer2 *ffp2) {
    if (!ffp2 || !ffp2->is)return -1;
    VideoState *is = ffp2->is;
    ff_packet_queue_destroy(&is->videoq);
    ff_packet_queue_destroy(&is->audioq);
    ff_packet_queue_destroy(&is->subtitleq);

    /* free all pictures */
    ff_frame_queue_destory(&is->pictq);
    ff_frame_queue_destory(&is->sampq);
    ff_frame_queue_destory(&is->subpq);
    sws_freeContext(is->img_convert_ctx);
    sws_freeContext(is->sub_convert_ctx);
    if (is->vis_texture)
        SDL_DestroyTexture(is->vis_texture);
    if (is->vid_texture)
        SDL_DestroyTexture(is->vid_texture);
    if (is->sub_texture)
        SDL_DestroyTexture(is->sub_texture);
    if (ffp2->video_renderer_context->video_renderer)
        SDL_DestroyRenderer(ffp2->video_renderer_context->ffDevice.renderer);
    if (ffp2->video_renderer_context->ffDevice.window)
        SDL_DestroyWindow(ffp2->video_renderer_context->ffDevice.window);
    ffp2->video_renderer_context->ffDevice.window = NULL;
    ffp2->video_renderer_context->video_renderer = NULL;
    ff_avrenderer_free_renderer_ctx(&ffp2->audio_renderer_context);
    ff_avrenderer_free_renderer_ctx(&ffp2->video_renderer_context);
    SDL_Quit();
    return 0;
}
