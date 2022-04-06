//
// Created by 阳坤 on 2022/3/24.
//

#ifndef FFPLAY2_FF_VIDEO_SDL_RENDERER_H
#define FFPLAY2_FF_VIDEO_SDL_RENDERER_H

#include "ff_struct_core.h"
#include "ff_frame_queue.h"

int video_open(struct FFplayer2 *ff);

int video_renderer(struct FFplayer2 *ff);

void set_default_window_size(int width, int height, int screen_width, int screen_height, int *default_width,
                             int *default_height, AVRational sar);


void toggle_full_screen(struct FFplayer2 *ff);


void create_sdl_window(const struct FFplayer2 *ff2, struct FFAVDevice *device);
#endif //FFPLAY2_FF_VIDEO_SDL_RENDERER_H
