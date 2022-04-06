#include "src/ff_ffplay.h"


static void sigterm_handler(int sig) {
    exit(123);
}

static void do_exit(struct FFplayer2 *ffp2){
    ffp2_destroy(ffp2);
    av_log(NULL, AV_LOG_QUIET, "%s", "");
    exit(0);
}

static /* handle an event sent by the GUI */
void sdl_streaam_loop(struct FFplayer2 *ffp2) {
    VideoState *cur_stream = ffp2->is;
    SDL_Event event;
    double incr, pos, frac;

    for (;;) {
        double x;
        ff_refresh_loop_wait_event(ffp2, &event); //video是在这里显示的
        switch (event.type) {
            case SDL_KEYDOWN:    /* 键盘事件 */
                if (ffp2->ffOptions->exit_on_keydown || event.key.keysym.sym == SDLK_ESCAPE ||
                    event.key.keysym.sym == SDLK_q) {
                    do_exit(ffp2);
                    break;
                }
                if (!cur_stream->width)
                    continue;
                switch (event.key.keysym.sym) {
                    case SDLK_f:
                        toggle_full_screen(cur_stream);
                        cur_stream->force_refresh = 1;
                        break;
                    case SDLK_p:
                    case SDLK_SPACE: //按空格键触发暂停/恢复
                        ffp2_pause(ffp2);
                        break;
                    case SDLK_m:
                        ffp2_mute(cur_stream);
                        break;
                    case SDLK_KP_MULTIPLY:
                    case SDLK_0:
                        ffp2_update_volume(cur_stream, 1, SDL_VOLUME_STEP);
                        break;
                    case SDLK_KP_DIVIDE:
                    case SDLK_9:
                        ffp2_update_volume(cur_stream, -1, SDL_VOLUME_STEP);
                        break;
                    case SDLK_s: // S: Step to next frame
                        ffp2_step_to_next_frame(cur_stream);
                        break;
                    case SDLK_a:
//                        stream_cycle_channel(cur_stream, AVMEDIA_TYPE_AUDIO);
                        break;
                    case SDLK_v:
//                        stream_cycle_channel(cur_stream, AVMEDIA_TYPE_VIDEO);
                        break;
                    case SDLK_c:
//                        stream_cycle_channel(cur_stream, AVMEDIA_TYPE_VIDEO);
//                        stream_cycle_channel(cur_stream, AVMEDIA_TYPE_AUDIO);
//                        stream_cycle_channel(cur_stream, AVMEDIA_TYPE_SUBTITLE);
                        break;
                    case SDLK_t:
//                        stream_cycle_channel(cur_stream, AVMEDIA_TYPE_SUBTITLE);
                        break;
                    case SDLK_w:
#if CONFIG_AVFILTER
                        if (cur_stream->show_mode == SHOW_MODE_VIDEO && cur_stream->vfilter_idx < nb_vfilters - 1) {
                    if (++cur_stream->vfilter_idx >= nb_vfilters)
                        cur_stream->vfilter_idx = 0;
                } else {
                    cur_stream->vfilter_idx = 0;
                    toggle_audio_display(cur_stream);
                }
#else
//                        toggle_audio_display(cur_stream);
#endif
                        break;
                    case SDLK_PAGEUP:
                        if (cur_stream->ic->nb_chapters <= 1) {
                            incr = 600.0;
                            goto do_seek;
                        }
//                        seek_chapter(cur_stream, 1);
                        break;
                    case SDLK_PAGEDOWN:
                        if (cur_stream->ic->nb_chapters <= 1) {
                            incr = -600.0;
                            goto do_seek;
                        }
//                        seek_chapter(cur_stream, -1);
                        break;
                    case SDLK_LEFT://测试未通过
                        incr = ffp2->ffOptions->seek_interval ? -ffp2->ffOptions->seek_interval : -20.0;
                        goto do_seek;
                    case SDLK_RIGHT:
                        incr = ffp2->ffOptions->seek_interval ? ffp2->ffOptions->seek_interval : 20.0;
                        goto do_seek;
                    case SDLK_UP:
                        incr = 60.0;
                        goto do_seek;
                    case SDLK_DOWN:
                        incr = -60.0;
                    do_seek:
                        if (ffp2->ffOptions->seek_by_bytes) {
                            pos = -1;
                            if (pos < 0 && cur_stream->video_stream >= 0)
                                pos = ff_frame_queue_last_pos(&cur_stream->pictq);
                            if (pos < 0 && cur_stream->audio_stream >= 0)
                                pos = ff_frame_queue_last_pos(&cur_stream->sampq);
                            if (pos < 0)
                                pos = avio_tell(cur_stream->ic->pb);
                            if (cur_stream->ic->bit_rate)
                                incr *= cur_stream->ic->bit_rate / 8.0;
                            else
                                incr *= 180000.0;
                            pos += incr;
                            ff_demuxer_stream_seek(ffp2, pos, incr, 1);
                        } else {
                            pos = ff_get_master_clock(cur_stream);
                            if (isnan(pos))
                                pos = (double) cur_stream->seek_pos / AV_TIME_BASE;
                            pos += incr;    // 现在是秒的单位
                            if (cur_stream->ic->start_time != AV_NOPTS_VALUE &&
                                pos < cur_stream->ic->start_time / (double) AV_TIME_BASE)
                                pos = cur_stream->ic->start_time / (double) AV_TIME_BASE;


                            av_log(NULL, AV_LOG_DEBUG, "stream_seek->pos=%ld rel=%ld \n",
                                   (int64_t) (pos * AV_TIME_BASE), (int64_t) (incr * AV_TIME_BASE));
                            ff_demuxer_stream_seek(ffp2, (int64_t) (pos * AV_TIME_BASE), (int64_t) (incr * AV_TIME_BASE), 0);
                        }
                        break;
                    default:
                        break;
                }
                break;
            case SDL_MOUSEBUTTONDOWN:            /* 鼠标按下事件 */
                if (ffp2->ffOptions->exit_on_mousedown) {
                    ffp2_destroy(ffp2);
                    break;
                }
                if (event.button.button == SDL_BUTTON_LEFT) {
                    static int64_t last_mouse_left_click = 0;
                    if (av_gettime_relative() - last_mouse_left_click <= 500000) {
                        //连续鼠标左键点击2次显示窗口间隔小于0.5秒，则进行全屏或者恢复原始窗口
                        toggle_full_screen(cur_stream);
                        cur_stream->force_refresh = 1;
                        last_mouse_left_click = 0;
                    } else {
                        last_mouse_left_click = av_gettime_relative();
                    }
                }
            case SDL_MOUSEMOTION:        /* 鼠标移动事件 */
                if (ffp2->ffOptions->cursor_hidden) {
                    SDL_ShowCursor(1);
                    ffp2->ffOptions->cursor_hidden = 0;
                }
                ffp2->ffOptions->cursor_last_shown = av_gettime_relative();
                if (event.type == SDL_MOUSEBUTTONDOWN) {
                    if (event.button.button != SDL_BUTTON_RIGHT)
                        break;
                    x = event.button.x;
                } else {
                    if (!(event.motion.state & SDL_BUTTON_RMASK))
                        break;
                    x = event.motion.x;
                }
                if (ffp2->ffOptions->seek_by_bytes || cur_stream->ic->duration <= 0) {
                    uint64_t size = avio_size(cur_stream->ic->pb); // 整个文件的字节
                    ff_demuxer_stream_seek(ffp2, size * x / cur_stream->width, 0, 1);
                } else {
                    int64_t ts;
                    int ns, hh, mm, ss;
                    int tns, thh, tmm, tss;
                    tns = cur_stream->ic->duration / 1000000LL;
                    thh = tns / 3600;
                    tmm = (tns % 3600) / 60;
                    tss = (tns % 60);
                    frac = x / cur_stream->width;
                    ns = frac * tns;
                    hh = ns / 3600;
                    mm = (ns % 3600) / 60;
                    ss = (ns % 60);
                    av_log(NULL, AV_LOG_INFO,
                           "Seek to %2.0f%% (%2d:%02d:%02d) of total duration (%2d:%02d:%02d)       \n", frac * 100,
                           hh, mm, ss, thh, tmm, tss);
                    ts = frac * cur_stream->ic->duration;
                    if (cur_stream->ic->start_time != AV_NOPTS_VALUE)
                        ts += cur_stream->ic->start_time;
                    ff_demuxer_stream_seek(ffp2, ts, 0, 0);
                }
                break;
            case SDL_WINDOWEVENT:        /* 窗口事件 */
                switch (event.window.event) {
                    case SDL_WINDOWEVENT_SIZE_CHANGED:
                        ffp2->ffOptions->screen_width = cur_stream->width = event.window.data1;
                        ffp2->ffOptions->screen_height = cur_stream->height = event.window.data2;
                        if (cur_stream->vis_texture) {
                            SDL_DestroyTexture(cur_stream->vis_texture);
                            cur_stream->vis_texture = NULL;
                        }
                    case SDL_WINDOWEVENT_EXPOSED:
                        cur_stream->force_refresh = 1;
                }
                break;
            case SDL_QUIT:
            case FF_QUIT_EVENT:    /* ffplay自定义事件,用于主动退出 */
                do_exit(ffp2);
                break;
            default:
                break;
        }
    }
}

int main(int argc, char **argv) {
    //    捕获用户停止行为
    signal(SIGINT, sigterm_handler); /* Interrupt (ANSI).    */
    signal(SIGTERM, sigterm_handler); /* Termination (ANSI).  */
    if (argc == 2) {
        //1. 创建一个播放器对象,并设置播放路径
        FFplayer2 *ffp2 = ffp2_create();
        if (!ffp2)return -1;
        ffp2->is->filename = av_strdup(argv[1]);
        //2. 初始化播放器
        int ret = ffp2_init(ffp2);
        if (ret == 0) {
            //3. 开始播放
            ffp2_start(ffp2);
            sdl_streaam_loop(ffp2);
        }
    }
    return 0;
}
