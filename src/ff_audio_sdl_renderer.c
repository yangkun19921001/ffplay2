//
// Created by 阳坤 on 2022/3/24.
//

#include "ff_audio_sdl_renderer.h"

/**
 * @brief 如果sync_type是视频或外部主时钟，则返回所需样本数以获得更好的同步
 * @param is
 * @param nb_samples    正常播放的采样数量
 * @return
 */
static int synchronize_audio(VideoState *is, int nb_samples) {
    int wanted_nb_samples = nb_samples;

    /* if not master, then we try to remove or add samples to correct the clock */
    if (ff_get_master_sync_type(is) != AV_SYNC_AUDIO_MASTER) {
        double diff, avg_diff;
        int min_nb_samples, max_nb_samples;

        diff = ff_get_clock(&is->audclk) - ff_get_master_clock(is);

        if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD) {
            // 误差在AV_NOSYNC_THRESHOLD 范围再来看看要不要调整
            is->audio_diff_cum = diff + is->audio_diff_avg_coef * is->audio_diff_cum;
            if (is->audio_diff_avg_count < AUDIO_DIFF_AVG_NB) {
                /* not enough measures to have a correct estimate */
                is->audio_diff_avg_count++; // 连续20次不同步才进行校正
            } else {
                /* estimate the A-V difference */
                avg_diff = is->audio_diff_cum * (1.0 - is->audio_diff_avg_coef);
//                avg_diff = diff;
                if (fabs(avg_diff) >= is->audio_diff_threshold) {
                    wanted_nb_samples = nb_samples + (int) (diff * is->audio_src.freq);
                    min_nb_samples = ((nb_samples * (100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                    max_nb_samples = ((nb_samples * (100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                    // av_clip 用来限制wanted_nb_samples最终落在 min_nb_samples~max_nb_samples
                    // nb_samples *（90%~110%）
                    wanted_nb_samples = av_clip(wanted_nb_samples, min_nb_samples, max_nb_samples);
                }
                av_log(NULL, AV_LOG_INFO, "diff=%f adiff=%f sample_diff=%d apts=%0.3f %f\n",
                       diff, avg_diff, wanted_nb_samples - nb_samples,
                       is->audio_clock, is->audio_diff_threshold);
            }
        } else {
            // > AV_NOSYNC_THRESHOLD 阈值，该干嘛就干嘛
            /* too big difference : may be initial PTS errors, so
               reset A-V filter */
            is->audio_diff_avg_count = 0;
            is->audio_diff_cum = 0;   // 恢复正常后重置为0
        }
    }

    return wanted_nb_samples;
}


/**
 * Decode one audio frame and return its uncompressed size.
 *
 * The processed audio frame is decoded, converted if required, and
 * stored in is->audio_buf, with size in bytes given by the return
 * value.
 */
static int audio_decode_frame(VideoState *is) {
    int data_size, resampled_data_size;
    int64_t dec_channel_layout;
    av_unused double audio_clock0;
    int wanted_nb_samples;
    Frame *af;

    if (is->paused)
        return -1;

    do {
#if defined(_WIN32)
        while (frame_queue_nb_remaining(&is->sampq) == 0) {
            if ((av_gettime_relative() - audio_callback_time) > 1000000LL * is->audio_hw_buf_size / is->audio_tgt.bytes_per_sec / 2)
                return -1;
            av_usleep (1000);
        }
#endif
        // 若队列头部可读，则由af指向可读帧
        if (!(af = ff_frame_queue_peek_readable(&is->sampq)))
            return -1;
        ff_frame_queue_next(&is->sampq);
    } while (af->serial != is->audioq.serial);

    // 根据frame中指定的音频参数获取缓冲区的大小 af->frame->channels * af->frame->nb_samples * 2
    data_size = av_samples_get_buffer_size(NULL,
                                           af->frame->channels,
                                           af->frame->nb_samples,
                                           af->frame->format, 1);
    // 获取声道布局
    dec_channel_layout =
            (af->frame->channel_layout &&
             af->frame->channels == av_get_channel_layout_nb_channels(af->frame->channel_layout)) ?
            af->frame->channel_layout : av_get_default_channel_layout(af->frame->channels);
    //todo 获取样本数校正值：若同步时钟是音频，则不调整样本数；否则根据同步需要调整样本数
    wanted_nb_samples = synchronize_audio(is, af->frame->nb_samples);
    // is->audio_tgt是SDL可接受的音频帧数，是audio_open()中取得的参数
    // 在audio_open()函数中又有"is->audio_src = is->audio_tgt""
    // 此处表示：如果frame中的音频参数 == is->audio_src == is->audio_tgt，
    // 那音频重采样的过程就免了(因此时is->swr_ctr是NULL)
    // 否则使用frame(源)和is->audio_tgt(目标)中的音频参数来设置is->swr_ctx，
    // 并使用frame中的音频参数来赋值is->audio_src
    if (af->frame->format != is->audio_src.fmt || // 采样格式
        dec_channel_layout != is->audio_src.channel_layout || // 通道布局
        af->frame->sample_rate != is->audio_src.freq || // 采样率
        // 第4个条件, 要改变样本数量, 那就是需要初始化重采样
        (wanted_nb_samples != af->frame->nb_samples && !is->swr_ctx) // samples不同且swr_ctx没有初始化
            ) {
        swr_free(&is->swr_ctx);
        is->swr_ctx = swr_alloc_set_opts(NULL,
                                         is->audio_tgt.channel_layout,  // 目标输出
                                         is->audio_tgt.fmt,
                                         is->audio_tgt.freq,
                                         dec_channel_layout,            // 数据源
                                         af->frame->format,
                                         af->frame->sample_rate,
                                         0, NULL);
        if (!is->swr_ctx || swr_init(is->swr_ctx) < 0) {
            av_log(NULL, AV_LOG_ERROR,
                   "Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
                   af->frame->sample_rate, av_get_sample_fmt_name(af->frame->format), af->frame->channels,
                   is->audio_tgt.freq, av_get_sample_fmt_name(is->audio_tgt.fmt), is->audio_tgt.channels);
            swr_free(&is->swr_ctx);
            return -1;
        }
        is->audio_src.channel_layout = dec_channel_layout;
        is->audio_src.channels = af->frame->channels;
        is->audio_src.freq = af->frame->sample_rate;
        is->audio_src.fmt = af->frame->format;
    }

    if (is->swr_ctx) {
        // 重采样输入参数1：输入音频样本数是af->frame->nb_samples
        // 重采样输入参数2：输入音频缓冲区
        const uint8_t **in = (const uint8_t **) af->frame->extended_data; // data[0] data[1]

        // 重采样输出参数1：输出音频缓冲区尺寸
        uint8_t **out = &is->audio_buf1; //真正分配缓存audio_buf1，指向是用audio_buf
        // 重采样输出参数2：输出音频缓冲区
        int out_count = (int64_t) wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate
                        + 256;

        int out_size = av_samples_get_buffer_size(NULL, is->audio_tgt.channels,
                                                  out_count, is->audio_tgt.fmt, 0);
        int len2;
        if (out_size < 0) {
            av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size() failed\n");
            return -1;
        }
        // 如果frame中的样本数经过校正，则条件成立
        if (wanted_nb_samples != af->frame->nb_samples) {
            int sample_delta = (wanted_nb_samples - af->frame->nb_samples) * is->audio_tgt.freq
                               / af->frame->sample_rate;
            int compensation_distance = wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate;
            // swr_set_compensation
            if (swr_set_compensation(is->swr_ctx,
                                     sample_delta,
                                     compensation_distance) < 0) {
                av_log(NULL, AV_LOG_ERROR, "swr_set_compensation() failed\n");
                return -1;
            }
        }
        av_fast_malloc(&is->audio_buf1, &is->audio_buf1_size, out_size);
        if (!is->audio_buf1)
            return AVERROR(ENOMEM);
        // 音频重采样：返回值是重采样后得到的音频数据中单个声道的样本数
        len2 = swr_convert(is->swr_ctx, out, out_count, in, af->frame->nb_samples);
        if (len2 < 0) {
            av_log(NULL, AV_LOG_ERROR, "swr_convert() failed\n");
            return -1;
        }
        if (len2 == out_count) {
            av_log(NULL, AV_LOG_WARNING, "audio buffer is probably too small\n");
            if (swr_init(is->swr_ctx) < 0)
                swr_free(&is->swr_ctx);
        }
        // 重采样返回的一帧音频数据大小(以字节为单位)
        is->audio_buf = is->audio_buf1;
        resampled_data_size = len2 * is->audio_tgt.channels * av_get_bytes_per_sample(is->audio_tgt.fmt);
    } else {
        // 未经重采样，则将指针指向frame中的音频数据
        is->audio_buf = af->frame->data[0]; // s16交错模式data[0], fltp data[0] data[1]
        resampled_data_size = data_size;
    }

    audio_clock0 = is->audio_clock;
    /* update the audio clock with the pts */
    if (!isnan(af->pts))
        //更新音频 pts
        is->audio_clock = af->pts + (double) af->frame->nb_samples / af->frame->sample_rate;
    else
        is->audio_clock = NAN;
    is->audio_clock_serial = af->serial;
#ifdef DEBUG
    {
        static double last_clock;
        printf("audio: delay=%0.3f clock=%0.3f clock0=%0.3f\n",
               is->audio_clock - last_clock,
               is->audio_clock, audio_clock0);
        last_clock = is->audio_clock;
    }
#endif
    return resampled_data_size;
}

/* copy samples for viewing in editor window */
static void update_sample_display(VideoState *is, short *samples, int samples_size) {
    int size, len;

    size = samples_size / sizeof(short);
    while (size > 0) {
        len = SAMPLE_ARRAY_SIZE - is->sample_array_index;
        if (len > size)
            len = size;
        memcpy(is->sample_array + is->sample_array_index, samples, len * sizeof(short));
        samples += len;
        is->sample_array_index += len;
        if (is->sample_array_index >= SAMPLE_ARRAY_SIZE)
            is->sample_array_index = 0;
        size -= len;
    }
}


/* prepare a new audio buffer */
/**
 * @brief sdl_audio_callback
 * @param opaque    指向user的数据
 * @param stream    拷贝PCM的地址
 * @param len       需要拷贝的长度
 */
static void sdl_audio_callback(void *opaque, Uint8 *stream, int len) {
    FFplayer2 *ff = opaque;
    VideoState *is = ff->is;
    int audio_size, len1;
    ff->ffOptions->audio_callback_time = av_gettime_relative(); // while可能产生延迟

    while (len > 0) {   // 循环读取，直到读取到足够的数据
        /* (1)如果is->audio_buf_index < is->audio_buf_size则说明上次拷贝还剩余一些数据，
         * 先拷贝到stream再调用audio_decode_frame
         * (2)如果audio_buf消耗完了，则调用audio_decode_frame重新填充audio_buf
         */
        if (is->audio_buf_index >= is->audio_buf_size) {
            audio_size = audio_decode_frame(is);
            if (audio_size < 0) {
                /* if error, just output silence */
                is->audio_buf = NULL;
                is->audio_buf_size = SDL_AUDIO_MIN_BUFFER_SIZE / is->audio_tgt.frame_size
                                     * is->audio_tgt.frame_size;
            } else {
                if (is->show_mode != SHOW_MODE_VIDEO)
                    update_sample_display(is, (int16_t *) is->audio_buf, audio_size);
                is->audio_buf_size = audio_size; // 讲字节 多少字节
            }
            is->audio_buf_index = 0;
        }
        //根据缓冲区剩余大小量力而行
        //is->audio_buf_size -> 解码一帧的 buf size
        len1 = is->audio_buf_size - is->audio_buf_index;
        if (len1 > len)  // len = 3000 < len1 4096
            len1 = len;
        //根据audio_volume决定如何输出audio_buf
        /* 判断是否为静音，以及当前音量的大小，如果音量为最大则直接拷贝数据 */
        if (!is->muted && is->audio_buf && is->audio_volume == SDL_MIX_MAXVOLUME)
            memcpy(stream, (uint8_t *) is->audio_buf + is->audio_buf_index, len1);
        else {
            memset(stream, 0, len1);
            // 3.调整音量
            /* 如果处于mute状态则直接使用stream填0数据, 暂停时is->audio_buf = NULL */
            if (!is->muted && is->audio_buf)
                SDL_MixAudioFormat(stream, (uint8_t *) is->audio_buf + is->audio_buf_index,
                                   AUDIO_S16SYS, len1, is->audio_volume);
        }
        len -= len1;
        stream += len1;
        /* 更新is->audio_buf_index，指向audio_buf中未被拷贝到stream的数据（剩余数据）的起始位置 */
        is->audio_buf_index += len1;
    }

    is->audio_write_buf_size = is->audio_buf_size - is->audio_buf_index;
    /* Let's assume the audio driver that is used by SDL has two periods. */
    if (!isnan(is->audio_clock)) {
        double ap = is->audio_clock - ((double) (2 * is->audio_hw_buf_size + is->audio_write_buf_size)
                                       / is->audio_tgt.bytes_per_sec);
        printf("audio_clock=%lf pts=%f "
               "audio_hw_buf_size=%d "
               "audio_write_buf_size=%d "
               "audio_buf_size=%d "
               "audio_buf_index=%d "
               "bytes_per_sec=%d "
               "audio_clock_serial=%d "
               "audio_callback_time=%f "
               "\n",
               is->audio_clock, ap,
               is->audio_hw_buf_size,
               is->audio_write_buf_size,
               is->audio_buf_size,
               is->audio_buf_index,
               is->audio_tgt.bytes_per_sec,
               is->audio_clock_serial,
               ff->ffOptions->audio_callback_time / 1000000.0

        );
        //更新音频播放时间 -> 当前音频 pts - 剩余未播放
        //is->audio_clock          -> 当前音频解码 pts + 显示持续时长
        //is->audio_hw_buf_size    -> SDL音频缓冲区的大小(字节为单位)
        //is->audio_write_buf_size ->
        ff_set_clock_at(&is->audclk, is->audio_clock -
                                     (double) (2 * is->audio_hw_buf_size + is->audio_write_buf_size)
                                     / is->audio_tgt.bytes_per_sec,
                        is->audio_clock_serial,
                        ff->ffOptions->audio_callback_time / 1000000.0);
        ff_sync_clock_to_slave(&is->extclk, &is->audclk);
    }

}


int audio_open(struct FFplayer2 *ff) {
    VideoState *is = ff->is;
    AVCodecParameters *acs = is->ic->streams[is->st_index[AVMEDIA_TYPE_AUDIO]]->codecpar;
    int64_t wanted_channel_layout = acs->channel_layout;
    int wanted_nb_channels = acs->channels;
    int wanted_sample_rate = acs->sample_rate;

    AudioParams *audio_hw_params = &is->audio_tgt;

    SDL_AudioSpec wanted_spec, spec;

    const char *env;
    static const int next_nb_channels[] = {0, 0, 1, 6, 2, 6, 4, 6};
    static const int next_sample_rates[] = {0, 44100, 48000, 96000, 192000};
    int next_sample_rate_idx = FF_ARRAY_ELEMS(next_sample_rates) - 1;

    env = SDL_getenv("SDL_AUDIO_CHANNELS");
    if (env) {  // 若环境变量有设置，优先从环境变量取得声道数和声道布局
        wanted_nb_channels = atoi(env);
        wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
    }

    if (!wanted_channel_layout || wanted_nb_channels != av_get_channel_layout_nb_channels(wanted_channel_layout)) {
        wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
        wanted_channel_layout &= ~AV_CH_LAYOUT_STEREO_DOWNMIX;
    }

    // 根据channel_layout获取nb_channels，当传入参数wanted_nb_channels不匹配时，此处会作修正
    wanted_nb_channels = av_get_channel_layout_nb_channels(wanted_channel_layout);
    wanted_spec.channels = wanted_nb_channels;
    wanted_spec.freq = wanted_sample_rate;
    if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) {
        av_log(NULL, AV_LOG_ERROR, "Invalid sample rate or channel count!\n");
        return -1;
    }

    while (next_sample_rate_idx && next_sample_rates[next_sample_rate_idx] >= wanted_spec.freq)
        next_sample_rate_idx--;  // 从采样率数组中找到第一个不大于传入参数wanted_sample_rate的值
    // 音频采样格式有两大类型：planar和packed，假设一个双声道音频文件，一个左声道采样点记作L，一个右声道采样点记作R，则：
    // planar存储格式：(plane1)LLLLLLLL...LLLL (plane2)RRRRRRRR...RRRR
    // packed存储格式：(plane1)LRLRLRLR...........................LRLR
    // 在这两种采样类型下，又细分多种采样格式，如AV_SAMPLE_FMT_S16、AV_SAMPLE_FMT_S16P等，
    // 注意SDL2.0目前不支持planar格式
    // channel_layout是int64_t类型，表示音频声道布局，每bit代表一个特定的声道，参考channel_layout.h中的定义，一目了然
    // 数据量(bits/秒) = 采样率(Hz) * 采样深度(bit) * 声道数
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.silence = 0;
    /*
 * 一次读取多长的数据
 * SDL_AUDIO_MAX_CALLBACKS_PER_SEC一秒最多回调次数，避免频繁的回调
 *  Audio buffer size in samples (power of 2)
 */
    wanted_spec.samples = FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE,
                                2 << av_log2(wanted_spec.freq / SDL_AUDIO_MAX_CALLBACKS_PER_SEC));
    wanted_spec.callback = sdl_audio_callback;
    wanted_spec.userdata = ff;

    // 打开音频设备并创建音频处理线程。期望的参数是wanted_spec，实际得到的硬件参数是spec
    // 1) SDL提供两种使音频设备取得音频数据方法：
    //    a. push，SDL以特定的频率调用回调函数，在回调函数中取得音频数据
    //    b. pull，用户程序以特定的频率调用SDL_QueueAudio()，向音频设备提供数据。此种情况wanted_spec.callback=NULL
    // 2) 音频设备打开后播放静音，不启动回调，调用SDL_PauseAudio(0)后启动回调，开始正常播放音频
    // SDL_OpenAudioDevice()第一个参数为NULL时，等价于SDL_OpenAudio()
    while (!(ff->audio_renderer_context->ffDevice.audio_dev = SDL_OpenAudioDevice(NULL, 0, &wanted_spec, &spec,
                                                                                  SDL_AUDIO_ALLOW_FREQUENCY_CHANGE |
                                                                                  SDL_AUDIO_ALLOW_CHANNELS_CHANGE))) {
        av_log(NULL, AV_LOG_WARNING, "SDL_OpenAudio (%d channels, %d Hz): %s\n",
               wanted_spec.channels, wanted_spec.freq, SDL_GetError());
        wanted_spec.channels = next_nb_channels[FFMIN(7, wanted_spec.channels)];
        if (!wanted_spec.channels) {
            wanted_spec.freq = next_sample_rates[next_sample_rate_idx--];
            wanted_spec.channels = wanted_nb_channels;
            if (!wanted_spec.freq) {
                av_log(NULL, AV_LOG_ERROR,
                       "No more combinations to try, audio open failed\n");
                return -1;
            }
        }
        wanted_channel_layout = av_get_default_channel_layout(wanted_spec.channels);
    }
    // 检查打开音频设备的实际参数：采样格式
    if (spec.format != AUDIO_S16SYS) {
        av_log(NULL, AV_LOG_ERROR,
               "SDL advised audio format %d is not supported!\n", spec.format);
        return -1;
    }

    // 检查打开音频设备的实际参数：声道数
    if (spec.channels != wanted_spec.channels) {
        wanted_channel_layout = av_get_default_channel_layout(spec.channels);
        if (!wanted_channel_layout) {
            av_log(NULL, AV_LOG_ERROR,
                   "SDL advised channel count %d is not supported!\n", spec.channels);
            return -1;
        }
    }

    // wanted_spec是期望的参数，spec是实际的参数，wanted_spec和spec都是SDL中的结构。
    // 此处audio_hw_params是FFmpeg中的参数，输出参数供上级函数使用
    // audio_hw_params保存的参数，就是在做重采样的时候要转成的格式。
    audio_hw_params->fmt = AV_SAMPLE_FMT_S16;
    audio_hw_params->freq = spec.freq;
    audio_hw_params->channel_layout = wanted_channel_layout;
    audio_hw_params->channels = spec.channels;

    /* audio_hw_params->frame_size这里只是计算一个采样点占用的字节数 */
    audio_hw_params->frame_size = av_samples_get_buffer_size(NULL, audio_hw_params->channels,
                                                             1, audio_hw_params->fmt, 1);
    audio_hw_params->bytes_per_sec = av_samples_get_buffer_size(NULL, audio_hw_params->channels,
                                                                audio_hw_params->freq,
                                                                audio_hw_params->fmt, 1);

    if (audio_hw_params->bytes_per_sec <= 0 || audio_hw_params->frame_size <= 0) {
        av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size failed\n");
        return -1;
    }
    // 比如2帧数据，一帧就是1024个采样点， 1024*2*2 * 2 = 8192字节
    return spec.size;    /* SDL内部缓存的数据字节, samples * channels *byte_per_sample */

}