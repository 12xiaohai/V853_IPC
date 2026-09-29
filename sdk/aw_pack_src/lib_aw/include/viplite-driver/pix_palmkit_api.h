/**************************************************************************
 *
 * Copyright (c) 2018-2023 by Guangzhou Pixtalks Information Technology Co., Ltd
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

#ifndef __PIXTALKS_PIX_PALMKIT_API_V1_H__
#define __PIXTALKS_PIX_PALMKIT_API_V1_H__

#include "pix_algokit_public_type.h"

#ifdef __cplusplus
extern "C" {
#endif

#if PIX_BUILD_LEVEL == PIX_BUILD_LEVEL_INNER_ROC_TEST
void pix_inner_set_img_name(const char *img_name);
void pix_inner_get_hand_landmark(float *hand_landmark_3d);
void pix_inner_get_hand_liveness1(float *hand_liveness1);
#endif //PIX_BUILD_LEVEL

int pix_set_product_id(pix_board_product_id_e product_id);

// 2.1.0 @since v1.0
const char *pix_palmkit_version();

int pix_init_detect_model(const pix_auth_info_t *auth_info, const unsigned char *pack_model_ptr,
                           uint32_t pack_model_size_byte);

//
int pix_commone_detect(const unsigned char *nir_img_ptr, pix_coord_info_t *coord_info, pix_detect_target_type_e *target_type);

// 2.1.1 @since v1.0
int pix_get_pr_feature_bytes();

// 2.1.2 @since v1.0
float pix_palm_liveness_threshold();

// 2.1.3 @since v1.0
float pix_pr_threshold();

// 2.1.4 @since v1.0
#ifndef RELEASE_FACEKIT_SDK_API // 若已经提供了人脸识别算法，人脸识别算法SDK里已经提供了获取硬件信息的接口
int pix_get_hardware_info(uint8_t *hardware_info, uint32_t hardware_info_bytes);
#endif //RELEASE_FACEKIT_SDK_API



// 2.1.5 @since v1.0
int pix_init_palmkit_model(const pix_auth_info_t *auth_info, const unsigned char *pack_model_ptr, uint32_t pack_model_size_byte);

// 2.1.6 @since v1.0
int pix_release_palmkit_model();

// 对于单IR或单RGB, img1_ptr、img2_ptr传相同的值
// 对于IR+RGB双目, img1_ptr传NIR图, img2_ptr传RGB图
// 对于IR+IR双目, img1_ptr传左图, img2_ptr传右图(左/右图的定义参考pix_set_camera_k说明)
// 对于结构光, img1_ptr传泛光NIR图, img2_ptr传原始光斑图
// 2.1.8 @since v1.0
int pix_set_normal_analyze_image_palm(const unsigned char *img1_ptr, const unsigned char *img2_ptr);

// 2.1.10 @since v1.0
int pix_get_palm_liveness_score(int palm_index, float *liveness_score);

int pix_is_palm_quality_good_for_register(int palm_index, int *is_good);

// 2.1.11 @since v1.0
int pix_get_pr_feature(int palm_index, int is_register_flag, pix_pr_feature_info_t *face_pr_feature);

// 2.1.12 @since v1.0
int pix_cal_fea_sim_palm(const pix_pr_feature_info_t *fea1, const pix_pr_feature_info_t *fea2, float *pr_score);

// 2.1.13 @since v1.0
// 特定版本才会开放次接口
int pix_partner_debug_data_palm(unsigned char *debug_data_ptr, int *debug_data_bytes);

#ifdef __cplusplus
}
#endif

#endif  // __PIXTALKS_PIX_PALMKIT_API_V1_H__
