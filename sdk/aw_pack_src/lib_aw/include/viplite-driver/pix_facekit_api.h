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

#ifndef __PIXTALKS_PIX_FACEKIT_API_V3_H__
#define __PIXTALKS_PIX_FACEKIT_API_V3_H__

#include "pix_algokit_public_type.h"


#ifdef __cplusplus
extern "C" {
#endif

int set_fd_param(int img_w, int img_h, pix_target_orientation_e face_orientation, float threshold);

// 1.1
const char *pix_facekit_version();

// 2.1.1
int pix_set_algo_type(pix_algo_type_t algo_type);

// 2.1.2 todo
int pix_set_fd_param(pix_img_type_t img_type, int img_w, int img_h, pix_target_orientation_e face_orientation, float threshold);

// 2.1.3 双目模组需要提供相机内参. (以人面向镜头为参考, 左边摄像头为left_camera, 右边摄像头为right_camera. 左边摄像头出图为左图, 右摄像头出图为右图)
int pix_set_camera_k(float *left_camera_k, float *right_camera_k);

// todo
void pix_close_image_quality_checking(int flag);

// 1.2
int pix_get_fr_feature_bytes();

// 1.3
float pix_get_liveness_threshold();

// 1.4
float pix_get_fr_threshold();

// 1.5
int pix_get_hardware_info(uint8_t *hardware_info, uint32_t hardware_info_bytes);

// 2.1.8
int pix_init_facekit_model(const pix_auth_info_t *auth_info, const unsigned char *pack_model_ptr, uint32_t pack_model_size_byte);

// 2.1.9
int pix_release_facekit_model();

// 2.1.10
int pix_init_facekit_model_lazy_part(const pix_auth_info_t *auth_info, const unsigned char *pack_model_ptr, uint32_t pack_model_size_byte);

// 2.1.11
int pix_release_facekit_model_lazy_part();

// 4.1
// if detect multi boxes, only return the biggest pixel area one
int pix_fast_fd(const unsigned char *nir_img_ptr, pix_coord_info_t *coord_info);

// 对于单IR或单RGB, img1_ptr、img2_ptr传相同的值
// 对于IR+RGB双目, img1_ptr传NIR图, img2_ptr传RGB图
// 对于IR+IR双目, img1_ptr传左图, img2_ptr传右图(左/右图的定义参考pix_set_camera_k说明)
// 对于结构光, img1_ptr传泛光NIR图, img2_ptr传光斑图或深度图
// 2.1.13
int pix_set_normal_analyze_image(const unsigned char *img1_ptr, const unsigned char *img2_ptr);

// 2.1.14
int pix_normal_fd(pix_coord_info_t *coord_info);

// 2.1.15
int pix_liveness_check(int face_index, float *liveness_score);

// 2.1.16
int pix_get_face_fr_feature(int face_index, int is_register_flag, pix_fr_feature_info_t *face_fr_feature);

// 2.1.17
int pix_with_mask_confidence(int face_index, float *mask_score);

// 2.1.18
int pix_get_face_attr_info(int face_index, pix_face_attr_info_t *face_attr);

// 2.1.19
int pix_cal_fea_sim(const pix_fr_feature_info_t *fea1, const pix_fr_feature_info_t *fea2, float *fr_score);

// 2.1.20
// 特定版本才会开放次接口
int pix_partner_debug_data(unsigned char *debug_data_ptr, int *debug_data_bytes);


#ifdef __cplusplus
}
#endif

#endif  // __PIXTALKS_PIX_FACEKIT_API_V3_H__
