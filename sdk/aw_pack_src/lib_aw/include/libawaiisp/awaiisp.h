/******************************************************************************
 Copyright (C), 2001-2023, Allwinner Tech. Co., Ltd.
******************************************************************************
 File Name     : awaiisp.h
 Version       : Initial Draft
 Author        : Allwinner
 Created       : 2023/10/16
 Last Modified :
 Description   : aiisp function
 Function List :
 History       :
******************************************************************************/
#ifndef _AWAIISP_
#define _AWAIISP_

#ifdef __cplusplus
       extern "C" {
#endif

/**
  error code
*/
#define AWAIISP_SUCCESS                       (0x00000000)
#define ERR_AWAIISP_BUSY                      (0x00000001)
#define ERR_AWAIISP_NO_MEM                    (0x00000002)
#define ERR_AWAIISP_NULL_PTR                  (0x00000004)
#define ERR_AWAIISP_SYS_NOTREADY              (0x00000008)
#define ERR_AWAIISP_INVALID_DEVID             (0x00000010)
#define ERR_AWAIISP_ILLEGAL_PARAM             (0x00000011)
#define ERR_AWAIISP_NOT_SUPPORT               (0x00000012)

#define AWAIISP_FILE_PATH_MAX  (100)

typedef enum awaiisp_mode {
    /**
      Specify the npu mode, video frames processed by aiisp.
    */
    AWAIISP_MODE_NPU,
    /**
      Specify the normal mode, video frames are not processed by aiisp.
    */
    AWAIISP_MODE_NORMAL,
    /**
      Specify the normal gamma mode, video frames are not processed by aiisp, but keep gamma processing.
      When using this mode, needs to be configured ai_isp_en=1 for isp.
    */
    AWAIISP_MODE_NORMAL_GAMMA,
    /**
      Invalid parameter.
    */
    AWAIISP_MODE_LAST
} awaiisp_mode;

typedef struct awaiisp_config_param {
    /**
      Specify the path to the npu lut(gamma) model file.
    */
    char lut_model_file[AWAIISP_FILE_PATH_MAX];
    /**
      Specify the path to the model file.
    */
    char model_file[AWAIISP_FILE_PATH_MAX];
    /**
      Specify the width of the video frame.
    */
    int width;
    /**
      Specify the height of the video frame.
    */
    int height;
    /**
      Specify the tdm rx buf cnt, default 5.
    */
    int tdm_rxbuf_cnt;
    /**
      Specify the buf size when npu init, default 3MB. unit: bytes.
    */
    int npu_init_buf_size;
    /**
      Specify whether to open ion mem dev in libawaiisp, default:0.
    */
    int ion_mem_open;
    /**
      Specify aiisp mode, default:AWAIISP_MODE_NPU.
    */
    awaiisp_mode mode;
    /**
      Specify unprepared aiisp resources in advance for switching modes, if the current mode is not AWAIISP_MODE_NPU.
      It is must be set to 0 if the current mode is in AWAIISP_MODE_NPU mode
      default:0, means prepared.
    */
    int unprepared_aiisp_resources_advance;
    /**
      reserve params.
    */
    int reserve0;
    int reserve1;
} awaiisp_config_param;

typedef struct awaiisp_switch_param {
    /**
      aiisp switch mode.
    */
    awaiisp_mode mode;
    /**
      aiisp switch api blocking timeout time, unit is ms.
    */
    int timeout;
    /**
      Specify release aiisp resources when from AWAIISP_MODE_NPU to other mode, default:0, means not release.
    */
    int release_aiisp_resources;
} awaiisp_switch_param;


/**
  Define aiisp tdm_buffer_process_callback.

  @param param
    callback param.
*/
void awaiisp_tdm_buffer_process_callback(void *param);

/**
  Register aiisp return_tdm_buffer_callback.

  @param isp
    isp dev id.
  @param callback
    callback function.
  @return
    0: success
    others: fail, defined by error code
*/
int awaiisp_register_return_tdm_buffer_callback(int isp, void *callback);

/**
  Open aiisp and config params.

  @param isp
    isp dev id.
  @param config
    aiisp config params, defined by struct awaiisp_config_param.
  @return
    0: success
    others: fail, defined by error code
*/
int awaiisp_open(int isp, awaiisp_config_param *config);

/**
  Close aiisp.

  @param isp
    isp dev id.
  @return
    0: success
    others: fail, defined by error code
*/
int awaiisp_close(int isp);

/**
  Start aiisp.

  @param isp
    isp dev id.
  @return
    0: success
    others: fail, defined by error code
*/
int awaiisp_start(int isp);

/**
  Stop aiisp.

  @param isp
    isp dev id.
  @return
    0: success
    others: fail, defined by error code
*/
int awaiisp_stop(int isp);

/**
  This is a block api that needs to wait for its switch to complete before returning.

  @param isp
    isp dev id.
  @param switch_param
    aiisp switch params, defined by struct awaiisp_switch_param.
  @return
    0: success
    others: fail, defined by error code
*/
int awaiisp_switch_mode(int isp, awaiisp_switch_param *switch_param);

/**
  debug functions
*/
int awaiisp_register_dump_tdm_data_once_callback(int isp, void *callback);
void awaiisp_set_dump_g2d_data_once_path(int isp, char *filepath);

#ifdef __cplusplus
       }
#endif

#endif
