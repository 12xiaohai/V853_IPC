/******************************************************************************
  Copyright (C), 2020-2022, Allwinner Tech. Co., Ltd.
 ******************************************************************************
  File Name     : VencParameters.h
  Version       : Initial Draft
  Author        : Allwinner PDC-PD5 Team
  Created       : 2020/11/10
  Last Modified :
  Description   :
  Function List :
  History       :
******************************************************************************/
#ifndef __VNECPARAMETERS_H__
#define __VNECPARAMETERS_H__

#include <string>
#include <list>
#include <map>

#include <mm_common.h>
#include <mm_comm_video.h>
#include <mm_comm_venc.h>
#include <media_common_vcodec.h>
#include <vencoder.h>

#include <Errors.h>
#include <type_camera.h>

namespace EyeseeLinux {

class VencParameters
{
public:
    /*
    enum class VideoEncodeProductMode
    {
        NORMAL_MODE = 0,
        IPC_MODE = 1,
    };

    enum VideoEncodeRateControlMode
    {
        VideoRCMode_CBR = 0,
        VideoRCMode_VBR = 1,
        VideoRCMode_FIXQP = 2,
        VideoRCMode_ABR = 3,
        VideoRCMode_QPMAP = 4,
    };

    enum VEncProfile
    {
        VEncProfile_BaseLine = 0,
        VEncProfile_MP = 1,
        VEncProfile_HP = 2,
        VEncProfile_SVC = 3,
    };

    struct VEncBitRateControlAttr
    {
        PAYLOAD_TYPE_E          mVEncType;
        VideoEncodeRateControlMode  mRcMode;
        struct VEncAttrH264Cbr
        {
            unsigned int    mBitRate;
            unsigned int    mMaxQp;
            unsigned int    mMinQp;
            int mMaxPqp;    //default:50
            int mMinPqp;    //default:10
            int mQpInit;    //default:30
            int mbEnMbQpLimit;  //default:0
        };
        struct VEncAttrH264Vbr
        {
            unsigned int    mMaxBitRate;
            unsigned int    mMaxQp;
            unsigned int    mMinQp;
            int mMaxPqp;    //default:50
            int mMinPqp;    //default:10
            int mQpInit;    //default:30
            int mbEnMbQpLimit;  //default:0
            unsigned int    mMovingTh;
            int             mQuality;
            int mIFrmBitsCoef;  //default:15
            int mPFrmBitsCoef;  //default:10
        };
        struct VEncAttrH264FixQp
        {
            unsigned int    mIQp;
            unsigned int    mPQp;
        };
        struct VEncAttrH264Abr
        {
            unsigned int    mMaxBitRate;
            unsigned int    mRatioChangeQp;
            int             mQuality;
            unsigned int    mMinIQp;
            unsigned int    mMaxIQp;
            unsigned int    mMaxQp;
            unsigned int    mMinQp;
        };
        struct VEncAttrMjpegCbr
        {
            unsigned int    mBitRate;
        };
        struct VEncAttrMjpegFixQp
        {
            unsigned int    mQfactor;
        };
        typedef struct VEncAttrH264Cbr   VEncAttrH265Cbr;
        typedef struct VEncAttrH264Vbr   VEncAttrH265Vbr;
        typedef struct VEncAttrH264FixQp VEncAttrH265FixQp;
        typedef struct VEncAttrH264Abr   VEncAttrH265Abr;
        union
        {
            VEncAttrH264Cbr     mAttrH264Cbr;
            VEncAttrH264Vbr     mAttrH264Vbr;
            VEncAttrH264FixQp   mAttrH264FixQp;
            VEncAttrH264Abr     mAttrH264Abr;
            VEncAttrMjpegCbr    mAttrMjpegCbr;
            VEncAttrMjpegFixQp  mAttrMjpegFixQp;
            VEncAttrH265Cbr     mAttrH265Cbr;
            VEncAttrH265Vbr     mAttrH265Vbr;
            VEncAttrH265FixQp   mAttrH265FixQp;
            VEncAttrH265Abr     mAttrH265Abr;
        };
        unsigned int GetBitRate();
    };

    struct VEncAttr
    {
        PAYLOAD_TYPE_E  mType;
        unsigned int    mBufSize;
        unsigned int    mThreshSize;
        struct VEncAttrH264
        {
            unsigned int mProfile;  // 0:BL 1:MP 2:HP
            H264_LEVEL_E mLevel;
        };
        struct VEncAttrH265
        {
            unsigned int mProfile;  //0:MP
            H265_LEVEL_E mLevel;
        };
        union
        {
            VEncAttrH264 mAttrH264;
            VEncAttrH265 mAttrH265;
        };
    };
    */
    union VUI
    {
        VENC_PARAM_H264_VUI_S H264Vui;
        VENC_PARAM_H265_VUI_S H265Vui;
    };
    VencParameters();
    ~VencParameters();

    inline void setVideoFrameRate(int rate)
    {
        mFrameRate = rate;
    }
    inline int getVideoFrameRate()
    {
        return mFrameRate;
    }

    //void setVEncAttr(VEncAttr &pVEncAttr);
    //VEncAttr getVEncAttr();

    /*
    inline void setVideoEncoder(PAYLOAD_TYPE_E video_encoder)
    {
        mVideoEncoder = video_encoder;
    }
    */
    inline PAYLOAD_TYPE_E getVideoEncoder()
    {
        return mVEncChnAttr.VeAttr.Type;
    }

    void setVideoSize(const SIZE_S &stVideoSize);
    void getVideoSize(SIZE_S &stVideoSize);
    void setVideoEncodingBitRate(int bitRate);
    int getVideoEncodingBitRate();

    /**
      set I frame interal.
      @param nMaxKeyItl
        e.g., 30 stands for 30fps.
    */
    inline void setVideoEncodingIFramesNumberInterVal(int nMaxKeyItl)
    {
        mVEncChnAttr.VeAttr.MaxKeyInterval = nMaxKeyItl;
    }
    inline int getVideoEncodingIFramesNumberInterVal()
    {
        return mVEncChnAttr.VeAttr.MaxKeyInterval;
    }

    //status_t setIQpOffset(int nIQpOffset);
    inline int getIQpOffset()
    {
        return GetIQpOffsetFromVENC_CHN_ATTR_S(&mVEncChnAttr);
    }
    /*
    inline void enableFastEncode(bool enable)
    {
        mFastEncFlag = enable;
    }
    */
    inline bool getFastEncodeFlag()
    {
        return (bool)GetFastEncFlagFromVENC_CHN_ATTR_S(&mVEncChnAttr);
    }

    /*inline void enableVideoEncodingPIntra(bool enable)
    {
        mbPIntraEnable = enable;
    }*/
    inline bool getVideoEncodingPIntraFlag()
    {
        return (bool)GetPIntraEnableFromVENC_CHN_ATTR_S(&mVEncChnAttr);
    }

    /*inline void setOnlineEnable(bool enable)
    {
        mOnlineEnable = enable;
    }*/
    inline bool getOnlineEnable()
    {
        return (bool)mVEncChnAttr.VeAttr.mOnlineEnable;
    }

    /*inline void setOnlineShareBufNum(int num)
    {
        mOnlineShareBufNum = num;
    }*/
    inline int getOnlineShareBufNum()
    {
        return mVEncChnAttr.VeAttr.mOnlineShareBufNum;
    }

    //void setVEncBitRateControlAttr(VEncBitRateControlAttr &RcAttr);
    //VEncBitRateControlAttr getVEncBitRateControlAttr();

    //void setVideoEncodingRateControlMode(const VideoEncodeRateControlMode &rcMode);
    VENC_RC_MODE_E getVideoEncodingRateControlMode();

    //void setVideoEncodingProductMode(VideoEncodeProductMode &pdMode);
    eVencProductMode getVideoEncodingProductMode();

    /*inline void setSensorType(eSensorType eType)
    {
        mSensorType = eType;
    }*/
    /*inline eSensorType getSensorType()
    {
        return mVEncRcParam.sensor_type;
    }*/

    inline void enableNullSkip(bool enable)
    {
        mNullSkipEnable = enable;
    }
    inline bool getNullSkipFlag()
    {
        return mNullSkipEnable;
    }

    inline void enablePSkip(bool enable)
    {
        mPSkipEnable = enable;
    }
    inline bool getPSkipFlag()
    {
        return mPSkipEnable;
    }

    inline void enableHorizonFlip(bool enable)
    {
        mbHorizonfilp = enable;
    }
    inline bool getHorizonFilpFlag()
    {
        return mbHorizonfilp;
    }

    inline void enableAdaptiveIntraInp(bool enable)
    {
        mbAdaptiveintrainp = enable;
    }
    inline bool getAdaptiveIntraInpFlag()
    {
        return mbAdaptiveintrainp;
    }

    void set3DFilter(s3DfilterParam &n3DfilterParam);
    s3DfilterParam get3DFilter();

    void setVencSuperFrameConfig(VENC_SUPERFRAME_CFG_S &pSuperFrameConfig);
    VENC_SUPERFRAME_CFG_S getVencSuperFrameConfig();

    void enableSaveBSFile(VencSaveBSFile &nSavaParam);
    VencSaveBSFile getenableSaveBSFile();

    void setProcSet(VeProcSet &pVeProSet);
    VeProcSet getProcSet();

    inline void enableColor2Grey(bool enable)
    {
        mColor2Grey.bColor2Grey = (BOOL)enable;
    }
    inline bool getColor2GreyFlag()
    {
        return (bool)mColor2Grey.bColor2Grey;
    }

    void setVideoEncodingSmartP(VencSmartFun &pParam);
    VencSmartFun getVideoEncodingSmartP();

    void setVideoEncodingIntraRefresh(VENC_PARAM_INTRA_REFRESH_S &pIntraRefresh);
    VENC_PARAM_INTRA_REFRESH_S getVideoEncodingIntraRefresh();

    void setGopAttr(const VENC_GOP_ATTR_S &pParam);
    VENC_GOP_ATTR_S getGopAttr();

    void setRefParam(const VENC_PARAM_REF_S &pstRefParam);
    VENC_PARAM_REF_S getRefParam();

    inline void setVencChnIndex(const VENC_CHN nVeChn)
    {
        mVeChn = nVeChn;
    }
    inline VENC_CHN getVencChnIndex()
    {
        return mVeChn;
    }

    void setVencChnAttr(VENC_CHN_ATTR_S &nVencChnAttr);
    VENC_CHN_ATTR_S getVencChnAttr();
    
    void setVencRcParam(VENC_RC_PARAM_S &stVEncRcParam);
    VENC_RC_PARAM_S getVencRcParam();

    void setRoiCfg(VENC_ROI_CFG_S &pVencRoiCfg);
    VENC_ROI_CFG_S getRoiCfg();

    inline void setVui(VENC_PARAM_H264_VUI_S &vui)
    {
        mVuiInfo.H264Vui = vui;
    }
    inline void setVui(VENC_PARAM_H265_VUI_S &vui)
    {
        mVuiInfo.H265Vui = vui;
    }
    inline VUI getVui()
    {
        return mVuiInfo;
    }
    //status_t enableIframeFilter(bool enable);
    //bool getIframeFilter();

    //status_t setVideoEncodingMode(int Mode);

    //status_t setVideoSliceHeight(int sliceHeight);

    inline void enableIspAndVeLink(bool enable)
    {
        mbIspAndVeLinkEnable = enable;
    }
    inline bool getIspAndVeLinkEnable()
    {
        return mbIspAndVeLinkEnable;
    }

    inline void setMainStreamFlag(bool flag)
    {
        mbMainStreamFlag = flag;
    }
    inline bool getMainStreamFlag()
    {
        return mbMainStreamFlag;
    }

    inline void enableEncpp(bool enable)
    {
        mVEncChnAttr.EncppAttr.mbEncppEnable = (BOOL)enable;
    }
    inline bool getEncppEnable()
    {
        return (bool)mVEncChnAttr.EncppAttr.mbEncppEnable;
    }

    inline void setEncppSharpAttenCoefPer(int value)
    {
        mEncppSharpAttenCoefPer = value;
    }
    inline int getEncppSharpAttenCoefPer()
    {
        return mEncppSharpAttenCoefPer;
    }

private:
    int mFrameRate; //dst frame rate.
    //int mVideoWidth;
    //int mVideoHeight;
    //int mVideoMaxKeyItl; // 30fps:30,
    //int mIQpOffset;
    //int mFastEncFlag;
    //VideoEncodeProductMode mVideoPDMode;
    //eSensorType mSensorType;
    //bool mbPIntraEnable;
    bool mNullSkipEnable;
    bool mPSkipEnable;
    bool mbHorizonfilp;
    bool mbAdaptiveintrainp;
    s3DfilterParam mVenc3DnrParam;
    VENC_COLOR2GREY_S mColor2Grey;

    //PAYLOAD_TYPE_E mVideoEncoder;
    //VideoEncodeRateControlMode mVideoRCMode;
    //VEncAttr mVEncAttr;
    //VEncBitRateControlAttr mVEncRcAttr;
    VENC_CHN_ATTR_S mVEncChnAttr;
    VENC_RC_PARAM_S mVEncRcParam;
    VENC_PARAM_INTRA_REFRESH_S mIntraRefreshParam;
    VENC_PARAM_REF_S mVEncRefParam;
    VENC_ROI_CFG_S mVEncRoiCfg;
    VENC_SUPERFRAME_CFG_S mVEncSuperFrameCfg;
    VencSmartFun mSmartPParam;
    VUI mVuiInfo;
    VencSaveBSFile mSaveBSFileParam;
    VeProcSet mVeProcSet;

    VENC_CHN mVeChn;

    //bool mOnlineEnable;    /* 1: online, 0: offline.*/
    //int mOnlineShareBufNum; /* only for online. Number of share buffers of CSI and VE, support 0/1/2.*/

    //bool mbEncppEnable;
    int mEncppSharpAttenCoefPer; ///< user set Encpp sharp attenuation percentage coefficient.
    bool mbIspAndVeLinkEnable; ///< user set if enable isp2ve linkage
    bool mbMainStreamFlag; ///< user set if this venc stream is mainStream
};

};

#endif
