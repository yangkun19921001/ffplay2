//
// Created by 阳坤 on 2022/3/24.
//

#include "ff_video_sdl_renderer.h"

static unsigned sws_flags = SWS_BICUBIC;


static int
realloc_texture(SDL_Renderer *renderer, SDL_Texture **texture, Uint32 new_format, int new_width, int new_height,
                SDL_BlendMode blendmode, int init_texture) {
    Uint32 format;
    int access, w, h;
    if (!*texture || SDL_QueryTexture(*texture, &format, &access, &w, &h) < 0 || new_width != w || new_height != h ||
        new_format != format) {
        void *pixels;
        int pitch;
        if (*texture)
            SDL_DestroyTexture(*texture);
        if (!(*texture = SDL_CreateTexture(renderer, new_format, SDL_TEXTUREACCESS_STREAMING, new_width, new_height)))
            return -1;
        if (SDL_SetTextureBlendMode(*texture, blendmode) < 0)
            return -1;
        if (init_texture) {
            if (SDL_LockTexture(*texture, NULL, &pixels, &pitch) < 0)
                return -1;
            memset(pixels, 0, pitch * new_height);
            SDL_UnlockTexture(*texture);
        }
        av_log(NULL, AV_LOG_VERBOSE, "Created %dx%d texture with %s.\n", new_width, new_height,
               SDL_GetPixelFormatName(new_format));
    }
    return 0;
}

static void get_sdl_pix_fmt_and_blendmode(int format, Uint32 *sdl_pix_fmt, SDL_BlendMode *sdl_blendmode) {
    int i;
    *sdl_blendmode = SDL_BLENDMODE_NONE;
    *sdl_pix_fmt = SDL_PIXELFORMAT_UNKNOWN;
    if (format == AV_PIX_FMT_RGB32 ||
        format == AV_PIX_FMT_RGB32_1 ||
        format == AV_PIX_FMT_BGR32 ||
        format == AV_PIX_FMT_BGR32_1)
        *sdl_blendmode = SDL_BLENDMODE_BLEND;
    for (i = 0; i < FF_ARRAY_ELEMS(sdl_texture_format_map) - 1; i++) {
        if (format == sdl_texture_format_map[i].format) {
            *sdl_pix_fmt = sdl_texture_format_map[i].texture_fmt;
            return;
        }
    }
}


static int
upload_texture(SDL_Renderer *renderer, SDL_Texture **tex, AVFrame *frame, struct SwsContext **img_convert_ctx) {
    int ret = 0;
    Uint32 sdl_pix_fmt;
    SDL_BlendMode sdl_blendmode;
    // 根据frame中的图像格式(FFmpeg像素格式)，获取对应的SDL像素格式和blendmode
    get_sdl_pix_fmt_and_blendmode(frame->format, &sdl_pix_fmt, &sdl_blendmode);
    // 参数tex实际是&is->vid_texture，此处根据得到的SDL像素格式，为&is->vid_texture
    if (realloc_texture(renderer, tex, sdl_pix_fmt == SDL_PIXELFORMAT_UNKNOWN ? SDL_PIXELFORMAT_ARGB8888 : sdl_pix_fmt,
                        frame->width, frame->height, sdl_blendmode, 0) < 0)
        return -1;
    //根据sdl_pix_fmt从AVFrame中取数据填充纹理
    switch (sdl_pix_fmt) {
        // frame格式是SDL不支持的格式，则需要进行图像格式转换，转换为目标格式AV_PIX_FMT_BGRA，
        // 对应SDL_PIXELFORMAT_BGRA32
        case SDL_PIXELFORMAT_UNKNOWN:
            /* This should only happen if we are not using avfilter... */
            *img_convert_ctx = sws_getCachedContext(*img_convert_ctx,
                                                    frame->width, frame->height, frame->format,
                                                    frame->width, frame->height, AV_PIX_FMT_BGRA,
                                                    sws_flags, NULL, NULL, NULL);
            if (*img_convert_ctx != NULL) {
                uint8_t *pixels[4]; // 之前取Texture的缓存
                int pitch[4];
                if (!SDL_LockTexture(*tex, NULL, (void **) pixels, pitch)) {
                    sws_scale(*img_convert_ctx, (const uint8_t *const *) frame->data, frame->linesize,
                              0, frame->height, pixels, pitch);
                    SDL_UnlockTexture(*tex);
                }
            } else {
                av_log(NULL, AV_LOG_FATAL, "Cannot initialize the conversion context\n");
                ret = -1;
            }
            break;
            // frame格式对应SDL_PIXELFORMAT_IYUV，不用进行图像格式转换，调用SDL_UpdateYUVTexture()更新SDL texture
        case SDL_PIXELFORMAT_IYUV:
            if (frame->linesize[0] > 0 && frame->linesize[1] > 0 && frame->linesize[2] > 0) {
                ret = SDL_UpdateYUVTexture(*tex, NULL, frame->data[0], frame->linesize[0],
                                           frame->data[1], frame->linesize[1],
                                           frame->data[2], frame->linesize[2]);
            } else if (frame->linesize[0] < 0 && frame->linesize[1] < 0 && frame->linesize[2] < 0) {
                ret = SDL_UpdateYUVTexture(*tex, NULL, frame->data[0] + frame->linesize[0] * (frame->height - 1),
                                           -frame->linesize[0],
                                           frame->data[1] + frame->linesize[1] * (AV_CEIL_RSHIFT(frame->height, 1) - 1),
                                           -frame->linesize[1],
                                           frame->data[2] + frame->linesize[2] * (AV_CEIL_RSHIFT(frame->height, 1) - 1),
                                           -frame->linesize[2]);
            } else {
                av_log(NULL, AV_LOG_ERROR, "Mixed negative and positive linesizes are not supported.\n");
                return -1;
            }
            break;
            // frame格式对应其他SDL像素格式，不用进行图像格式转换，调用SDL_UpdateTexture()更新SDL texture
        default:
            if (frame->linesize[0] < 0) {
                ret = SDL_UpdateTexture(*tex, NULL, frame->data[0] + frame->linesize[0] * (frame->height - 1),
                                        -frame->linesize[0]);
            } else {
                ret = SDL_UpdateTexture(*tex, NULL, frame->data[0], frame->linesize[0]);
            }
            break;
    }
    return ret;
}


static void set_sdl_yuv_conversion_mode(AVFrame *frame) {
#if SDL_VERSION_ATLEAST(2, 0, 8)
    SDL_YUV_CONVERSION_MODE mode = SDL_YUV_CONVERSION_AUTOMATIC;
    if (frame && (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUYV422 ||
                  frame->format == AV_PIX_FMT_UYVY422)) {
        if (frame->color_range == AVCOL_RANGE_JPEG)
            mode = SDL_YUV_CONVERSION_JPEG;
        else if (frame->colorspace == AVCOL_SPC_BT709)
            mode = SDL_YUV_CONVERSION_BT709;
        else if (frame->colorspace == AVCOL_SPC_BT470BG || frame->colorspace == AVCOL_SPC_SMPTE170M ||
                 frame->colorspace == AVCOL_SPC_SMPTE240M)
            mode = SDL_YUV_CONVERSION_BT601;
    }
    SDL_SetYUVConversionMode(mode);
#endif
}

static void calculate_display_rect(SDL_Rect *rect,
                                   int scr_xleft, int scr_ytop, int scr_width, int scr_height,
                                   int pic_width, int pic_height, AVRational pic_sar) {
    AVRational aspect_ratio = pic_sar; // 比率
    int64_t width, height, x, y;

    if (av_cmp_q(aspect_ratio, av_make_q(0, 1)) <= 0)
        aspect_ratio = av_make_q(1, 1);// 如果aspect_ratio是负数或者为0,设置为1:1
    // 转成真正的播放比例
    aspect_ratio = av_mul_q(aspect_ratio, av_make_q(pic_width, pic_height));

    /* XXX: we suppose the screen has a 1.0 pixel ratio */
    // 计算显示视频帧区域的宽高
    // 先以高度为基准
    height = scr_height;
    // &~1, 取偶数宽度  1110
    width = av_rescale(height, aspect_ratio.num, aspect_ratio.den) & ~1;
    if (width > scr_width) {
        // 当以高度为基准,发现计算出来的需要的窗口宽度不足时调整为以窗口宽度为基准
        width = scr_width;
        height = av_rescale(width, aspect_ratio.den, aspect_ratio.num) & ~1;
    }
    // 计算显示视频帧区域的起始坐标（在显示窗口内部的区域）
    x = (scr_width - width) / 2;
    y = (scr_height - height) / 2;
    rect->x = scr_xleft + x;
    rect->y = scr_ytop + y;
    rect->w = FFMAX((int) width, 1);
    rect->h = FFMAX((int) height, 1);
}

static void video_audio_display(FFplayer2 *ff) {

}

static void video_image_display(FFplayer2 *ff) {
    VideoState *is = ff->is;
    Frame *vp;
    Frame *sp = NULL;
    SDL_Rect rect;

    // keep_last的作用就出来了,我们是有调用frame_queue_next, 但最近出队列的帧并没有真正销毁
    // 所以这里可以读取出来显示
    vp = ff_frame_queue_peek_last(&is->pictq); //
    if (is->subtitle_st) {
        if (ff_frame_queue_nb_remaining(&is->subpq) > 0) {
            sp = ff_frame_queue_peek(&is->subpq);

            if (vp->pts >= sp->pts + ((float) sp->sub.start_display_time / 1000)) {
                if (!sp->uploaded) {
                    uint8_t *pixels[4];
                    int pitch[4];
                    int i;
                    if (!sp->width || !sp->height) {
                        sp->width = vp->width;
                        sp->height = vp->height;
                    }
                    if (realloc_texture(ff->video_renderer_context->ffDevice.renderer, &is->sub_texture,
                                        SDL_PIXELFORMAT_ARGB8888, sp->width, sp->height,
                                        SDL_BLENDMODE_BLEND, 1) < 0)
                        return;

                    for (i = 0; i < sp->sub.num_rects; i++) {
                        AVSubtitleRect *sub_rect = sp->sub.rects[i];

                        sub_rect->x = av_clip(sub_rect->x, 0, sp->width);
                        sub_rect->y = av_clip(sub_rect->y, 0, sp->height);
                        sub_rect->w = av_clip(sub_rect->w, 0, sp->width - sub_rect->x);
                        sub_rect->h = av_clip(sub_rect->h, 0, sp->height - sub_rect->y);

                        is->sub_convert_ctx = sws_getCachedContext(is->sub_convert_ctx,
                                                                   sub_rect->w, sub_rect->h, AV_PIX_FMT_PAL8,
                                                                   sub_rect->w, sub_rect->h, AV_PIX_FMT_BGRA,
                                                                   0, NULL, NULL, NULL);
                        if (!is->sub_convert_ctx) {
                            av_log(NULL, AV_LOG_FATAL, "Cannot initialize the conversion context\n");
                            return;
                        }
                        if (!SDL_LockTexture(is->sub_texture, (SDL_Rect *) sub_rect, (void **) pixels, pitch)) {
                            sws_scale(is->sub_convert_ctx, (const uint8_t *const *) sub_rect->data, sub_rect->linesize,
                                      0, sub_rect->h, pixels, pitch);
                            SDL_UnlockTexture(is->sub_texture);
                        }
                    }
                    sp->uploaded = 1;
                }
            } else
                sp = NULL;
        }
    }
    //将帧宽高按照sar最大适配到窗口，并通过rect返回视频帧在窗口的显示位置和宽高
    calculate_display_rect(&rect, is->xleft, is->ytop, is->width, is->height,
                           vp->width, vp->height, vp->sar);
//    rect.x = rect.w /2;   // 测试
//    rect.w = rect.w /2;   // 缩放实际不是用sws， 缩放是sdl去做的
    if (!vp->uploaded) {
        // 把yuv数据更新到vid_texture
        if (upload_texture(ff->video_renderer_context->ffDevice.renderer, &is->vid_texture, vp->frame,
                           &is->img_convert_ctx) < 0)
            return;
        vp->uploaded = 1;
        vp->flip_v = vp->frame->linesize[0] < 0;
    }

    set_sdl_yuv_conversion_mode(vp->frame);
    SDL_RenderCopyEx(ff->video_renderer_context->ffDevice.renderer, is->vid_texture, NULL, &rect, 0, NULL,
                     vp->flip_v ? SDL_FLIP_VERTICAL : 0);
    set_sdl_yuv_conversion_mode(NULL);
    if (sp) {
#if USE_ONEPASS_SUBTITLE_RENDER
        SDL_RenderCopy(renderer, is->sub_texture, NULL, &rect);
#else
        int i;
        double xratio = (double) rect.w / (double) sp->width;
        double yratio = (double) rect.h / (double) sp->height;
        for (i = 0; i < sp->sub.num_rects; i++) {
            SDL_Rect *sub_rect = (SDL_Rect *) sp->sub.rects[i];
            SDL_Rect target = {.x = rect.x + sub_rect->x * xratio,
                    .y = rect.y + sub_rect->y * yratio,
                    .w = sub_rect->w * xratio,
                    .h = sub_rect->h * yratio};
            SDL_RenderCopy(ff->video_renderer_context->ffDevice.renderer, is->sub_texture, sub_rect, &target);
        }
#endif
    }
}


static void video_window_open(struct FFplayer2 *ff2) {
    int w, h;

    w = ff2->ffOptions->screen_width ? ff2->ffOptions->screen_width : ff2->ffOptions->default_width;
    h = ff2->ffOptions->screen_height ? ff2->ffOptions->screen_height : ff2->ffOptions->default_height;

    if (!ff2->ffOptions->window_title)
        ff2->ffOptions->window_title = ff2->ffOptions->input_filename;
    SDL_SetWindowTitle(ff2->video_renderer_context->ffDevice.window, ff2->ffOptions->window_title);

    SDL_SetWindowSize(ff2->video_renderer_context->ffDevice.window, w, h);
    SDL_SetWindowPosition(ff2->video_renderer_context->ffDevice.window, ff2->ffOptions->screen_left,
                          ff2->ffOptions->screen_top);
    if (ff2->ffOptions->is_full_screen)
        SDL_SetWindowFullscreen(ff2->video_renderer_context->ffDevice.window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    SDL_ShowWindow(ff2->video_renderer_context->ffDevice.window);

    ff2->is->width = w;
    ff2->is->height = h;
}

 void create_sdl_window(const struct FFplayer2 *ff2, struct FFAVDevice *device) {
    if (!ff2->ffOptions->display_disable) {
        int flags = SDL_WINDOW_HIDDEN;
        if (ff2->ffOptions->alwaysontop)
#if SDL_VERSION_ATLEAST(2, 0, 5)
            flags |= SDL_WINDOW_ALWAYS_ON_TOP;
#else
        av_log(NULL, AV_LOG_WARNING, "Your SDL version doesn't support SDL_WINDOW_ALWAYS_ON_TOP. Feature will be inactive.\n");
#endif
        if (ff2->ffOptions->borderless)
            flags |= SDL_WINDOW_BORDERLESS;
        else
            flags |= SDL_WINDOW_RESIZABLE;
        device->window = SDL_CreateWindow("program_name", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                          ff2->ffOptions->default_width,
                                          ff2->ffOptions->default_height, flags);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
        if (device->window) {
            // 创建renderer
            device->renderer = SDL_CreateRenderer(device->window, -1,
                                                  SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
            if (!device->renderer) {
                av_log(NULL, AV_LOG_WARNING, "Failed to initialize a hardware accelerated renderer: %s\n",
                       SDL_GetError());
                device->renderer = SDL_CreateRenderer(device->window, -1, 0);
            }
            if (device->renderer) {
                if (!SDL_GetRendererInfo(device->renderer, &device->renderer_info))
                    av_log(NULL, AV_LOG_VERBOSE, "Initialized %s renderer.\n", device->renderer_info.name);
            }
        }
        if (!device->window || !device->renderer || !device->renderer_info.num_texture_formats) {
            av_log(NULL, AV_LOG_FATAL, "Failed to create window or renderer: %s", SDL_GetError());
        }
    }
}

static void show_sdl_window(struct FFplayer2 *ff2) {
    VideoState *is = ff2->is;
    struct FFAVDevice *device = &ff2->video_renderer_context->ffDevice;
    if (!is->width) {
        // 4. 创建窗口
        create_sdl_window(ff2, device);
        //open 设备打开
        if (!ff2->video_renderer_context || !ff2->video_renderer_context->video_open ||
            (ff2->video_renderer_context->video_open(ff2)) < 0) {//如果窗口未显示，则显示窗口
            return;
        }
    }
}


/**
 * 视频设备打开
 * @param is
 * @return
 */
int video_open(struct FFplayer2 *ff) {
    VideoState *is = ff->is;
    is->ytop = 0;
    is->xleft = 0;
    video_window_open(ff);
    return is->width * is->height;
}


/**
 * 设置视频预览参数
 * @param width
 * @param height
 * @param screen_width
 * @param screen_height
 * @param default_width
 * @param default_height
 * @param sar
 */
void set_default_window_size(int width, int height, int screen_width, int screen_height, int *default_width,
                             int *default_height, AVRational sar) {
    SDL_Rect rect;
    int max_width = screen_width ? screen_width : INT_MAX;
    int max_height = screen_height ? screen_height : INT_MAX;
    if (max_width == INT_MAX && max_height == INT_MAX)
        max_height = height;
    calculate_display_rect(&rect, 0, 0, max_width, max_height, width, height, sar);
    *default_width = rect.w;
    *default_height = rect.h;
}

int video_renderer(struct FFplayer2 *ff) {
    show_sdl_window(ff);
    SDL_SetRenderDrawColor(ff->video_renderer_context->ffDevice.renderer, 0, 0, 0, 255);
    SDL_RenderClear(ff->video_renderer_context->ffDevice.renderer);
    if (ff->is->audio_st && ff->is->show_mode != SHOW_MODE_VIDEO)
        video_audio_display(ff);    //图形化显示仅有音轨的文件
    else if (ff->is->video_st)
        video_image_display(ff);    //显示一帧视频画面
    SDL_RenderPresent(ff->video_renderer_context->ffDevice.renderer);
    return 0;
}

void toggle_full_screen(struct FFplayer2 *ff) {
    ff->ffOptions->is_full_screen = !ff->ffOptions->is_full_screen;
    SDL_SetWindowFullscreen(ff->video_renderer_context->ffDevice.window,
                            ff->ffOptions->is_full_screen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}