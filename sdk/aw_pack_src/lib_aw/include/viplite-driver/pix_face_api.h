/**************************************************************************
 *
 * Copyright (c) 2021-2022 by Guangzhou Pixtalks Information Technology Co., Ltd
 *
 *  This software is copyrighted by and is the property of Guangzhou Pixtalks
 *  Information Technology Co., Ltd. All rights are reserved by Guangzhou
 *  Pixtalks Information Technology Co., Ltd. This software may only be used
 *  in accordance with the corresponding license agreement. Any unauthorized
 *  use, duplication, distribution, or disclosure of this software is
 *  expressly forbidden.
 *
 *  This Copyright notice MUST not be removed or modified without prior
 *  written consent of Guangzhou Pixtalks Information Technology Co., Ltd.
 *
 *  Guangzhou Pixtalks Information Technology Co., Ltd. reserves the right to
 *  modify this software without notice.
 *
 **************************************************************************/

#ifndef __PIXTALKS_PIX_FACE_API_V3_H__
#define __PIXTALKS_PIX_FACE_API_V3_H__

#include <inttypes.h>

#ifndef PIX_FR_FEATURE_BYTES
#define PIX_FR_FEATURE_BYTES (260)
#else

#if (PIX_FR_FEATURE_BYTES != 260)
PIX_FR_FEATURE_BYTES should be 260
#endif

#endif //PIX_FR_FEATURE_BYTES

#ifndef MAX_BOXES_NUM
#define MAX_BOXES_NUM 16
#else

#endif //MAX_BOXES_NUM


typedef struct pix_auth_info_s {
    const char *pix_licence;
} pix_auth_info_t;


#define API_PER_BOX_ELEMENTS 6
typedef struct pix_coord_info_s {
    int face_number;
    float face_box[API_PER_BOX_ELEMENTS * MAX_BOXES_NUM];
} pix_coord_info_t;


typedef struct pix_face_attr_info_s {
    float distance;  // mm
    float up_down;   // degree
    float left_right;
    float roll;
} pix_face_attr_info_t;

typedef struct pix_target_box_info_s{
    float target_box[API_PER_BOX_ELEMENTS];
}pix_target_box_info_t;

typedef struct pix_liveness_info_s{
    float liveness_score;
}pix_liveness_info_t;


typedef struct pix_fr_info_s {
    unsigned char fea[256];
} pix_fr_info_t;

typedef struct pix_fr_feature_info_s {
    unsigned char enc_key[16];
    unsigned int fea_length;
    unsigned char fea[PIX_FR_FEATURE_BYTES];
} pix_fr_feature_info_t;

typedef struct pix_image_face_info_s {
    pix_coord_info_t coord;
    pix_coord_info_t rgb_coord;
    pix_liveness_info_t liveness[MAX_BOXES_NUM];
    pix_fr_feature_info_t fr_feature[MAX_BOXES_NUM];
} pix_image_face_info_t;


#ifdef __cplusplus
extern "C" {
#endif


const char *pix_facekit_version();

int pix_get_fr_feature_bytes();

float pix_get_liveness_threshold();

float pix_get_fr_threshold();

int pix_get_hardware_info(uint8_t *hardware_info, uint32_t hardware_info_bytes);

int pix_init_facekit_model(unsigned char* auth_info,unsigned char *pack_model_ptr, unsigned char* key, int key_len);

int pix_release_facekit_model();

int pix_set_normal_analyze_image(const unsigned char *ir_img_ptr, const unsigned char *rgb_img_ptr);

int pix_fast_fd(const unsigned char *img_ptr, pix_coord_info_t *fd_info, int img_type);

int pix_normal_fd(pix_coord_info_t *fd_nir_info, pix_coord_info_t *fd_rgb_info);

int pix_set_normal_analyze_image(const unsigned char *nir_img_ptr, const unsigned char *rgb_img_ptr);

int get_biggest_face(pix_coord_info_t *fd_info, int * biggest_fd_index);

int pix_liveness_check(int face_index, float *liveness_score);

int pix_get_face_fr_feature(int face_index, pix_fr_feature_info_t *face_fr_feature, int is_register_feature);

int pix_cal_fea_sim(unsigned char *fea1, unsigned char *fea2, float *fr_score);

int pix_set_log_level(int log_level);

int pix_get_log_level();

void pix_set_camera_k(float *rgb_camera_k, float *nir_camera_k);


//int  isMatch_BGR_IR(FaceHandle bgr_handle, FaceHandle ir_handle, int &ret);

#ifdef __cplusplus
}
#endif

#endif  // __PIXTALKS_PIX_FACE_API_V3_H__
