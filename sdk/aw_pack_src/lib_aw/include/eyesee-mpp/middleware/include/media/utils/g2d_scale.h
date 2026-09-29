/** @file

  File Name     : g2d_scale.h
  Version       : Initial Draft
  Author        : eric_wang
  Created       : 2023/02/17
  Last Modified :
  Description   : use g2d to scale/convert picture size and format
  Function List :
  History       :
*/

#ifndef _G2D_SCALE_H_
#define _G2D_SCALE_H_

#include <mm_common.h>
#include <mm_comm_video.h>
#include <linux/videodev2.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef struct ScalePictureParam
{
    PIXEL_FORMAT_E eSrcPixFormat;
    enum v4l2_colorspace eSrcColorSpace; //V4L2_COLORSPACE_REC709, V4L2_COLORSPACE_REC709_PART_RANGE, enum user_v4l2_colorspace
    unsigned int mSrcPhyAddrs[3];
    SIZE_S mSrcPicSize;
    RECT_S mSrcValidRect;

    PIXEL_FORMAT_E eDstPixFormat;
    enum v4l2_colorspace eDstColorSpace; //V4L2_COLORSPACE_REC709, V4L2_COLORSPACE_REC709_PART_RANGE, enum user_v4l2_colorspace
    unsigned int mDstPhyAddrs[3];
    SIZE_S mDstPicSize;
    RECT_S mDstValidRect;
}ScalePictureParam;

int ScalePictureByG2d(ScalePictureParam *pScaleInfo, int nG2dFd);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif  /* _G2D_SCALE_H_ */

