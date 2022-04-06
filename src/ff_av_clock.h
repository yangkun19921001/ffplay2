//
// Created by 阳坤 on 2022/3/20.
//

#ifndef FFPLAY2_FF_AV_CLOCK_H
#define FFPLAY2_FF_AV_CLOCK_H

#include "ff_struct_core.h"

void ff_init_clock(struct Clock *c,int *queue_serial);

void ff_set_clock(struct Clock *c, double pts, int serial);

double ff_get_clock(struct Clock *c);

void ff_set_clock_at(struct Clock *c, double pts, int serial, double time);

void ff_set_clock_speed(struct Clock *c, double speed);

void ff_sync_clock_to_slave(struct Clock *c,struct Clock *slave);

int ff_get_master_sync_type(struct VideoState *is);

double ff_get_master_clock(struct VideoState *is);

void ff_check_external_clock_speed(struct VideoState *is);


#endif //FFPLAY2_FF_AV_CLOCK_H
