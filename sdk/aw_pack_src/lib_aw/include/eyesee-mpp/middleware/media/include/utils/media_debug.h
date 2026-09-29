/******************************************************************************
  Copyright (C), 2001-2022, Allwinner Tech. Co., Ltd.
 ******************************************************************************
  File Name     : media_debug.h
  Version       : Initial Draft
  Author        : Allwinner PDC-PD5 Team
  Created       : 2022/03/26
  Last Modified :
  Description   : media debug functions implement
  Function List :
  History       :
******************************************************************************/
#ifndef __MEDIA_DEBUG_H__
#define __MEDIA_DEBUG_H__

#include <videoInputHw.h>
#include <VideoEnc_Component.h>
#include <RecRender_Component.h>
#include <ConfigOption.h>

#if (MPPCFG_VENC == OPTION_VENC_ENABLE)
#define MEDIA_DEBUG_VENC_CONFIG_KEYFRAMEINTERVAL     (0x1)
#define MEDIA_DEBUG_VENC_CONFIG_BITRATE              (0x2)
#define MEDIA_DEBUG_VENC_CONFIG_2DNR                 (0x4)
#define MEDIA_DEBUG_VENC_CONFIG_3DNR                 (0x8)
#define MEDIA_DEBUG_VENC_CONFIG_COLOR2GREY           (0x10)
#define MEDIA_DEBUG_VENC_CONFIG_CROP                 (0x20)
#define MEDIA_DEBUG_VENC_CONFIG_SUPERFRM             (0x40)
#define MEDIA_DEBUG_VENC_CONFIG_MIRROR               (0x80)
#define MEDIA_DEBUG_VENC_CONFIG_REGIOND3D            (0x100)
#define MEDIA_DEBUG_VENC_CONFIG_CHROMAQPOFFSET       (0x200)
#define MEDIA_DEBUG_VENC_CONFIG_H264CONSTRAINTFLAG   (0x400)
#define MEDIA_DEBUG_VENC_CONFIG_D2DLIMIT             (0x800)
#define MEDIA_DEBUG_VENC_CONFIG_ENCPPENABLE          (0x1000)
#define MEDIA_DEBUG_VENC_CONFIG_VEREFFRAMELBCMODE    (0x2000)
#endif

#if (MPPCFG_VI == OPTION_VI_ENABLE)
void MediaDebugLoadMppViParams(viChnManager *pVipp, const char *pConfPath);
#endif
#if (MPPCFG_VENC == OPTION_VENC_ENABLE)
void MediaDebugLoadMppVencParams(VIDEOENCDATATYPE *pVideoEncData, const char *pConfPath);
#endif
#if (MPPCFG_MUXER == OPTION_MUXER_ENABLE)
void MediaDebugLoadMppMuxParams(RECRENDERDATATYPE *pRecRenderData, const char *pConfPath);
#endif

#endif /* __MEDIA_DEBUG_H__ */
