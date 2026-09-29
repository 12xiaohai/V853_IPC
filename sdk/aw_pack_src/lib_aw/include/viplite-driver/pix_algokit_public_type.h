#ifndef __PIXTALKS_PIX_ALGOKIT_PUBLIC_TYPE_H__
#define __PIXTALKS_PIX_ALGOKIT_PUBLIC_TYPE_H__

#include <inttypes.h>

#ifndef __PIX_QG310_PRODUCT_ID_E__
#define __PIX_QG310_PRODUCT_ID_E__
typedef enum pix_board_product_id_e_e{
    pix_qg310_product_id_127A = 0, //face
    pix_qg310_product_id_149A = 1, //face
    pix_qg310_product_id_149B = 2, //face+hand
    pix_qg310_product_id_149C = 3, //face
    pix_qg310_product_id_161A = 4, //ir+rgb face
    pix_qg310_product_id_161B = 5, //ir+ir face+hand
    pix_qg310_product_id_161C = 6, //ir+rgb face+hand
    pix_qg310_product_id_161D = 7, //ir+ir face

    pix_qg310_product_id_anan1 = 8, //ir+rgb face

    pix_product_id_face_only_ir_rgb = 9,
    pix_product_id_face_only_ir_ir = 10,
    pix_product_id_hand_only_ir_rgb = 11,
    pix_product_id_hand_only_ir_ir = 12,
    pix_product_id_face_and_hand_ir_rgb = 13,
    pix_product_id_face_and_hand_ir_ir = 14,
}pix_board_product_id_e;
#endif //__PIX_QG310_PRODUCT_ID_E__

#ifdef PIX_5007_HARDWARE_INFO_BYTES
#if PIX_5007_HARDWARE_INFO_BYTES != 64
#error "PIX_5007_HARDWARE_INFO_BYTES Must be 64" PIX_5007_HARDWARE_INFO_BYTES
#endif //PIX_5007_HARDWARE_INFO_BYTES
#else
#define PIX_5007_HARDWARE_INFO_BYTES 64
#endif //PIX_5007_HARDWARE_INFO_BYTES

#ifdef PIX_5007_LICENCE_BYTES
#if PIX_5007_LICENCE_BYTES != 128
#error "PIX_5007_LICENCE_BYTES Must be 128" PIX_5007_LICENCE_BYTES
#endif //PIX_5007_LICENCE_BYTES
#else
#define PIX_5007_LICENCE_BYTES 128
#endif //PIX_5007_LICENCE_BYTES

#ifdef FACE_KEY_POINT_NUM
#if (FACE_KEY_POINT_NUM != 5)
#error "Bad FACE_KEY_POINT_NUM ,expect 5"
#endif  // if
#else
#define FACE_KEY_POINT_NUM 5
#endif //FACE_KEY_POINT_NUM

#ifndef PIX_FR_FEATURE_BYTES
#define PIX_FR_FEATURE_BYTES 1024
#else
#if (PIX_FR_FEATURE_BYTES != 1024)
PIX_FR_FEATURE_BYTES should be 1024
#endif
#endif //PIX_FR_FEATURE_BYTES

#ifndef PIX_PR_FEATURE_BYTES
#define PIX_PR_FEATURE_BYTES 1600
#else

#if (PIX_PR_FEATURE_BYTES != 1600)
PIX_PR_FEATURE_BYTES should be 1600
#endif
#endif //PIX_PR_FEATURE_BYTES

#ifndef MAX_BOXES_NUM
#define MAX_BOXES_NUM 128
#else
#if MAX_BOXES_NUM != 128
#error MAX_BOXES_NUM must 128
#endif
#endif //MAX_BOXES_NUM

#ifndef API_PER_BOX_ELEMENTS
#define API_PER_BOX_ELEMENTS 5
#else
#if API_PER_BOX_ELEMENTS != 5
#error API_PER_BOX_ELEMENTS must 5
#endif
#endif //API_PER_BOX_ELEMENTS

#ifndef __PIX_BOOL_T___
#define __PIX_BOOL_T___
typedef uint8_t pix_bool_t;
#endif //__PIX_BOOL_T___

#ifndef __PIX_ALGO_TYPE_T__
#define __PIX_ALGO_TYPE_T__
typedef enum pix_algo_type_e {
    pix_algo_type_single_sensor_ir = 1,
    pix_algo_type_single_sensor_rgb,
    pix_algo_type_ir_rgb_stereo_sensor,
    pix_algo_type_ir_ir_stereo_sensor,
    pix_algo_type_structure_light_with_depth,
    pix_algo_type_structure_light_with_raw_pattern,
} pix_algo_type_t;
#endif //__PIX_ALGO_TYPE_T__

#ifndef __PIX_IMG_TYPE_T__
#define __PIX_IMG_TYPE_T__
typedef enum pix_img_type_e_e{
    pix_img_type_e_raw_gray8 = 0,
    pix_img_type_e_raw_16bit,
    pix_img_type_e_raw_rgb_interleave,
    pix_img_type_e_raw_yuv420uv,
    pix_img_type_e_raw_q12dot4,
    pix_img_type_e_encoded,
}pix_img_type_t;
#endif //__PIX_IMG_TYPE_T__

#ifndef __PIX_DETECT_TARGET_TYPE_T__
#define __PIX_DETECT_TARGET_TYPE_T__
typedef enum pix_detect_target_type_e_e{
    pix_detect_target_type_face = 0,
    pix_detect_target_type_body = 1,
    pix_detect_target_type_circle = 2,
    pix_detect_target_type_hand = 3,
    pix_detect_target_type_charging_plugin = 4,
    pix_detect_target_type_undefine = 100,
}pix_detect_target_type_e;
#endif // __PIX_DETECT_TARGET_TYPE_T__

#ifndef __PIX_IMG_INFO_T__
#define __PIX_IMG_INFO_T__
typedef struct pix_img_info_s{
    pix_img_type_t img_type;
    int img_c;
    int img_h;
    int img_w;
    void *data_ptr;
}pix_img_info_t;
#endif //__PIX_IMG_INFO_T__

#ifndef __EYE_TRACKING_INFO_T__
#define __EYE_TRACKING_INFO_T__
typedef struct eye_tracking_info_s {
    float distance;
    float up_down;
    float left_right;
    float roll;
    float face_box[5];
    float t2d_position[FACE_KEY_POINT_NUM * 2];
    float t3d_position[FACE_KEY_POINT_NUM * 3];
} eye_tracking_info_t;
#endif //__EYE_TRACKING_INFO_T__

#ifndef __PIX_TARGET_ORIENTATION_E__
#define __PIX_TARGET_ORIENTATION_E__
typedef enum pix_target_orientation_e_e {
    pix_target_orientation_on_top = 0,
    pix_target_orientation_on_bottom = 1,
    pix_target_orientation_on_left = 2,
    pix_target_orientation_on_right = 3
}pix_target_orientation_e;
#endif //__PIX_TARGET_ORIENTATION_E__

#ifndef __PIX_AUTH_INFO_T__
#define __PIX_AUTH_INFO_T__
typedef struct pix_auth_info_s {
    const char *pix_licence;
} pix_auth_info_t;
#endif //__PIX_AUTH_INFO_T__

#ifndef __PIX_COORD_INFO_T__
#define __PIX_COORD_INFO_T__
typedef struct pix_coord_info_s {
    int box_number;
    //每个目标物检测结果用5个元素表示（x1 y1 x2 y2 confidence），其中（x1,y1)像素坐标系下框的左上角坐标，(x2, y2)为右下角坐标, confidence为检测物的置信度（一般情况下，应用端无需理会改值）
    float box[API_PER_BOX_ELEMENTS * MAX_BOXES_NUM];
} pix_coord_info_t;
#endif //__PIX_COORD_INFO_T__

#ifndef __PIX_FACE_ATTR_INFO_T__
#define __PIX_FACE_ATTR_INFO_T__
typedef struct pix_face_attr_info_s {
    float distance;  // 单位mm
    float up_down;   // 单位角度，数值符号表示方向，数值绝对值表示角度大小，下同。抬头低头偏离角度（俯仰角）
    float left_right; // 左右偏离角度（偏航角）
    float roll; // 旋转角度
} pix_face_attr_info_t;
#endif //__PIX_FACE_ATTR_INFO_T__

#ifndef __PIX_LIVENESS_INFO_T__
#define __PIX_LIVENESS_INFO_T__
typedef struct pix_liveness_info_s{
    float score1;
}pix_liveness_info_t;
#endif //__PIX_LIVENESS_INFO_T__

#ifndef __PIX_FR_FEATURE_INFO_T__
#define __PIX_FR_FEATURE_INFO_T__
typedef struct pix_fr_feature_info_s {
    unsigned char enc_key[16];
    unsigned int fea_length;
    unsigned char fea[PIX_FR_FEATURE_BYTES]; //PIX_FR_FEATURE_BYTES值和平台相关,请以平台Demo代码定义值为准.
} pix_fr_feature_info_t;
#endif //__PIX_FR_FEATURE_INFO_T__

#ifndef __PIX_PR_FEATURE_INFO_T__
#define __PIX_PR_FEATURE_INFO_T__
typedef struct pix_pr_feature_info_s {
    unsigned char enc_key[16];
    unsigned int fea_length;
    unsigned char fea[PIX_PR_FEATURE_BYTES]; //PIX_PR_FEATURE_BYTES值和平台相关,请以平台Demo代码定义值为准.
} pix_pr_feature_info_t;
#endif //__PIX_PR_FEATURE_INFO_T__

#ifndef __PIX_IMAGE_FACE_INFO_T__
#define __PIX_IMAGE_FACE_INFO_T__
typedef struct pix_image_face_info_s {
    pix_coord_info_t coord; //4 + 4 * 5 * max_num
    pix_face_attr_info_t face_attr[MAX_BOXES_NUM]; // 4 * 4 * max_num
    float mask_score[MAX_BOXES_NUM]; // 4 * max_num
    pix_liveness_info_t liveness[MAX_BOXES_NUM]; // 4 * max_num
    pix_fr_feature_info_t fr_feature[MAX_BOXES_NUM]; // (16 + 4 + 1024) * max_num
} pix_image_face_info_t;
#endif //__PIX_IMAGE_FACE_INFO_T__

#ifndef __CAMERA_INFO_PARAM_T__
#define __CAMERA_INFO_PARAM_T__
typedef struct camera_info_param_s{
    const char *auth_code;
    int sensor1_img_w; // structure light flood image or stereo senor1
    int sensor1_img_h;
    int sensor2_img_w; // structure light speckle image or structure light 3D image or stereo senor2
    int sensor2_img_h;
    pix_target_orientation_e orientation; //
    void *inverse_ir_intrinsic_double_type;
}camera_info_param_t;
#endif //__CAMERA_INFO_PARAM_T__

#ifndef __PIX_POINT2F_T__
#define __PIX_POINT2F_T__
typedef struct  pix_point2f_s{
    float x;
    float y;
}pix_point2f_t;
#endif //__PIX_POINT2F_T__

#endif  // __PIXTALKS_PIX_ALGOKIT_PUBLIC_TYPE_H__
