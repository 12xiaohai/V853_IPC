/*********************************************************************/
//文件名: agc_float.h
//文件说明: agc算法头文件
//作者:
//生成日期: 2021-03-17
/*********************************************************************/
#ifndef _AGC_FLOAT_H_

#define _AGC_FLOAT_H_

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef struct
{
    int   i_sample_rate;
    int   i_channel;

    float f_attack_time;
    float f_release_time;

    float f_gs_pre[2];//2ch
    int siFirstEnter[2];

    float f_target_db;
    float f_max_gain_db;

    float x0;
    float y0;
    float x1;
    float y1;

    float r0;
    float r1;

    float f_attack;
    float f_release;
}agc_handle;


agc_handle *  func_agc_init(int i_sample_rate,
                            int     i_channel,
                            float   f_target_db,
                            float   f_max_gain_db);


int func_agc_proc(agc_handle *agc_handle_,
                    short *ps_pcm_In,
                    short *ps_pcm_out,
                    int ilen);


int func_agc_set_target_db(agc_handle *agc_handle_, float f_target_db);


int func_agc_set_maxgain_db(agc_handle *agc_handle_, float f_maxgain_db);


int func_agc_exit(agc_handle * agc_handle_);

#endif

