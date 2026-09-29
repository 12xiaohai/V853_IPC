#ifndef _MUXCACHEMANAGER_H_
#define _MUXCACHEMANAGER_H_

#include <string.h>

#include <vector>
#include <list>
#include <condition_variable>

#include <Errors.h>
#include "MuxStreamNode.h"

namespace EyeseeLinux {

const int CacheSourcePrefixFlag = 0x7EAB0000;
class EyeseeRecorder;
/**
  Mux cache manager is used to cache encode streams.
  We assume below scenario:
  total two threads call mux cache manager:
  1. EyeseeRecorder's mpSendStreamThread call pushStream(), controlCacheLevel().
  2. mpi_mux threads to call releaseStream().
*/
class MuxCacheManager
{
public:
    /**
      one streamBufManager manage one stream.
      we assume stream nodes are sequential requested and returned on stream buffer.
      StreamBufManager don't do mutex, we use MuxCacheManager's mutex to do mutex for all StreamBufManagers.
    */
    struct StreamBufManager
    {
        int mStreamId;
        char *mpBuf;
        int mnBufSize; //unit:bytes
        char* mpWritePos;
        char* mpReadPos;
        int mnValidLen;
        std::list<MuxStreamNode> mStreamNodeReadyList;
        std::list<MuxStreamNode> mStreamNodeUsingList;
        /**
          copy stream content to streamBufManager, then create a node in StreamBufManager's ready list.
          we guarantee space is enough to contain stream content.

          @return
            NO_ERROR
        */
        template<class T> status_t CopyToStreamBuf(const T& XencStream)
        {
            int nStreamLen = 0;
            if(std::is_same<T, VENC_STREAM_S>::value)
            {
                VENC_STREAM_S *pVencStream = (VENC_STREAM_S*)&XencStream;
                nStreamLen = pVencStream->mpPack[0].mLen0 + pVencStream->mpPack[0].mLen1 + pVencStream->mpPack[0].mLen2;
            }
            else if(std::is_same<T, AUDIO_STREAM_S>::value)
            {
                AUDIO_STREAM_S *pAencStream = (AUDIO_STREAM_S*)&XencStream;
                nStreamLen = pAencStream->mLen + pAencStream->mExtraLen;
            }
            else if(std::is_same<T, TEXT_STREAM_S>::value)
            {
                TEXT_STREAM_S *pTencStream = (TEXT_STREAM_S*)&XencStream;
                nStreamLen = pTencStream->mLen + pTencStream->mExtraLen;
            }
            else
            {
                aloge("fatal error! unknown T");
            }
            if(nStreamLen > mnBufSize - mnValidLen)
            {
                aloge("fatal error! streamBuf info:[%d-%d-%d-%d]", mStreamId, mnBufSize, mnValidLen, nStreamLen);
            }
            char *pDatas[3] = {NULL, NULL, NULL};
            int nDataLens[3] = {0, 0, 0};
            if(std::is_same<T, VENC_STREAM_S>::value)
            {
                VENC_STREAM_S *pVencStream = (VENC_STREAM_S*)&XencStream;
                pDatas[0] = (char*)pVencStream->mpPack[0].mpAddr0;
                pDatas[1] = (char*)pVencStream->mpPack[0].mpAddr1;
                pDatas[2] = (char*)pVencStream->mpPack[0].mpAddr2;
                nDataLens[0] = pVencStream->mpPack[0].mLen0;
                nDataLens[1] = pVencStream->mpPack[0].mLen1;
                nDataLens[2] = pVencStream->mpPack[0].mLen2;
            }
            else if(std::is_same<T, AUDIO_STREAM_S>::value)
            {
                AUDIO_STREAM_S *pAencStream = (AUDIO_STREAM_S*)&XencStream;
                pDatas[0] = (char*)pAencStream->pStream;
                pDatas[1] = (char*)pAencStream->pStreamExtra;
                nDataLens[0] = pAencStream->mLen;
                nDataLens[1] = pAencStream->mExtraLen;
            }
            else if(std::is_same<T, TEXT_STREAM_S>::value)
            {
                TEXT_STREAM_S *pTencStream = (TEXT_STREAM_S*)&XencStream;
                pDatas[0] = (char*)pTencStream->pStream;
                pDatas[1] = (char*)pTencStream->pStreamExtra;
                nDataLens[0] = pTencStream->mLen;
                nDataLens[1] = pTencStream->mExtraLen;
            }
            else
            {
                aloge("fatal error! unknown T");
            }
            char *pNodeBuf0 = mpWritePos;
            for(int i=0;i<3;i++) //copy every vencBuf to streamBuf
            {
                if(nDataLens[i] > 0)
                {
                    char *pFreeDatas[2] = {NULL, NULL};
                    int nFreeDataLens[2] = {0, 0};
                    if(mpWritePos == mpReadPos)
                    {
                        if(0==mnValidLen)
                        {
                            pFreeDatas[0] = mpWritePos;
                            nFreeDataLens[0] = mpBuf + mnBufSize - mpWritePos;
                            if(mpWritePos != mpBuf)
                            {
                                pFreeDatas[1] = mpBuf;
                                nFreeDataLens[1] = mpWritePos - mpBuf;
                            }
                        }
                        else
                        {
                            aloge("fatal error! streamId[%] Buf validLen:%d/%d", mStreamId, mnValidLen, mnBufSize);
                        }
                    }
                    else if(mpWritePos > mpReadPos)
                    {
                        pFreeDatas[0] = mpWritePos;
                        nFreeDataLens[0] = mpBuf + mnBufSize - mpWritePos;
                        if(mpReadPos != mpBuf)
                        {
                            pFreeDatas[1] = mpBuf;
                            nFreeDataLens[1] = mpReadPos - mpBuf;
                        }
                    }
                    else if(mpWritePos < mpReadPos)
                    {
                        pFreeDatas[0] = mpWritePos;
                        nFreeDataLens[0] = mpReadPos - mpWritePos;
                    }

                    for(int j=0;j<2;j++) //copy to two StreamBuf.
                    {
                        if(nDataLens[i] <= nFreeDataLens[j])
                        {
                            memcpy(pFreeDatas[j], pDatas[i], nDataLens[i]);
                            mpWritePos += nDataLens[i];
                            if(mpWritePos >= mpBuf + mnBufSize)
                            {
                                if(mpWritePos > mpBuf + mnBufSize)
                                {
                                    aloge("fatal error! why streamId[%d] Buf:[%p-%p-%d]", mStreamId, mpWritePos, mpBuf, mnBufSize);
                                }
                                mpWritePos = mpBuf;
                            }
                            mnValidLen += nDataLens[i];
                            break;
                        }
                        else
                        {
                            memcpy(pFreeDatas[j], pDatas[i], nFreeDataLens[j]);
                            mpWritePos += nFreeDataLens[j];
                            if(mpWritePos >= mpBuf + mnBufSize)
                            {
                                if(mpWritePos > mpBuf + mnBufSize)
                                {
                                    aloge("fatal error! why streamId[%d] Buf:[%p-%p-%d]", mStreamId, mpWritePos, mpBuf, mnBufSize);
                                }
                                mpWritePos = mpBuf;
                            }
                            mnValidLen += nFreeDataLens[j];
                            pDatas[i] += nFreeDataLens[j];
                            nDataLens[i] -= nFreeDataLens[j];
                            if(1 == j)
                            {
                                aloge("fatal error! streamId[%d] Buf[%d]:[%d-%d-%d]", mStreamId, i, nDataLens[i], nFreeDataLens[0], nFreeDataLens[1]);
                            }
                        }
                    }
                }
            }
            char *pNodeBuf1 = NULL;
            int nNodeBufLen0 = 0;
            int nNodeBufLen1 = 0;
            if(mpWritePos > pNodeBuf0)
            {
                nNodeBufLen0 = mpWritePos - pNodeBuf0;
                pNodeBuf1 = NULL;
                nNodeBufLen1 = 0;
            }
            else if(mpWritePos == pNodeBuf0)
            {
                if(pNodeBuf0 == mpBuf)
                {
                    nNodeBufLen0 = mnBufSize;
                    pNodeBuf1 = NULL;
                    nNodeBufLen1 = 0;
                }
                else
                {
                    nNodeBufLen0 = mpBuf + mnBufSize - pNodeBuf0;
                    pNodeBuf1 = mpBuf;
                    nNodeBufLen1 = mpWritePos - mpBuf;
                }
            }
            else
            {
                nNodeBufLen0 = mpBuf + mnBufSize - pNodeBuf0;
                pNodeBuf1 = mpBuf;
                nNodeBufLen1 = mpWritePos - mpBuf;
            }
            //add to ready list.
            mStreamNodeReadyList.emplace_back(XencStream, mStreamId);
            MuxStreamNode &newNode = mStreamNodeReadyList.back();
            if(MuxStreamNode::Type::Video == newNode.mStreamType)
            {
                newNode.mVencStream.mpPack[0].mpAddr0 = (unsigned char*)pNodeBuf0;
                newNode.mVencStream.mpPack[0].mpAddr1 = (unsigned char*)pNodeBuf1;
                newNode.mVencStream.mpPack[0].mpAddr2 = NULL;
                newNode.mVencStream.mpPack[0].mLen0 = nNodeBufLen0;
                newNode.mVencStream.mpPack[0].mLen1 = nNodeBufLen1;
                newNode.mVencStream.mpPack[0].mLen2 = 0;
                newNode.mVencStream.mSeq = (newNode.mVencStream.mSeq & 0xFFFF) | CacheSourcePrefixFlag;
            }
            else if(MuxStreamNode::Type::Audio == newNode.mStreamType)
            {
                newNode.mAencStream.pStream = (unsigned char*)pNodeBuf0;
                newNode.mAencStream.pStreamExtra = (unsigned char*)pNodeBuf1;
                newNode.mAencStream.mLen = nNodeBufLen0;
                newNode.mAencStream.mExtraLen = nNodeBufLen1;
                newNode.mAencStream.mId = (newNode.mAencStream.mId & 0xFFFF) | CacheSourcePrefixFlag;
            }
            else if(MuxStreamNode::Type::Text == newNode.mStreamType)
            {
                newNode.mTencStream.pStream = (unsigned char*)pNodeBuf0;
                newNode.mTencStream.pStreamExtra = (unsigned char*)pNodeBuf1;
                newNode.mTencStream.mLen = nNodeBufLen0;
                newNode.mTencStream.mExtraLen = nNodeBufLen1;
                newNode.mTencStream.mId = (newNode.mTencStream.mId & 0xFFFF) | CacheSourcePrefixFlag;
            }
            else
            {
                aloge("fatal error! unknown streamType:%d", newNode.mStreamType);
            }
            return NO_ERROR;
        }
        MuxStreamNode* PrefetchFirstStreamNode();
        MuxStreamNode* GetStreamNode();
        status_t RefStreamNode(MuxStreamNode *pStreamNode);
        status_t ReleaseStreamNode(int nNodeId);
        status_t ReleaseReadyStreamNode(int nNodeId);
        int GetDuration();
        StreamBufManager(int nStreamId, int nBufSize);
        ~StreamBufManager();
    private:
        status_t ReleaseStreamBuf(MuxStreamNode& streamNode);
    };
    std::list<StreamBufManager> mStreamBufManagerList;
    std::mutex mLock;
    std::condition_variable mcvReleaseUsingPacket;
    bool mbWaitReleasePacketFlag;
    status_t AddStreamBufManager(int nStreamId, int nBufSize);
    /**
      push one stream to buffer to store.
      support venc/aenc/tenc stream. If not enough memory, need wait here forever!

      @return
        NO_ERROR
        UNKNOWN_ERROR
    */
    template<class T> status_t PushStream(const T& XencStream, int nStreamId)
    {
        std::unique_lock<std::mutex> autoLock(mLock);
        std::list<StreamBufManager>::iterator it;
        for(it=mStreamBufManagerList.begin(); it!=mStreamBufManagerList.end(); ++it)
        {
            if(it->mStreamId == nStreamId)
            {
                break;
            }
        }
        if(it == mStreamBufManagerList.end())
        {
            alogv("not cache streamId[%d]", nStreamId);
            return UNKNOWN_ERROR;
        }
        int nStreamLen = 0;
        if(std::is_same<T, VENC_STREAM_S>::value)
        {
            VENC_STREAM_S *pVencStream = (VENC_STREAM_S*)&XencStream;
            nStreamLen = pVencStream->mpPack[0].mLen0 + pVencStream->mpPack[0].mLen1 + pVencStream->mpPack[0].mLen2;
        }
        else if(std::is_same<T, AUDIO_STREAM_S>::value)
        {
            AUDIO_STREAM_S *pAencStream = (AUDIO_STREAM_S*)&XencStream;
            nStreamLen = pAencStream->mLen + pAencStream->mExtraLen;
        }
        else if(std::is_same<T, TEXT_STREAM_S>::value)
        {
            TEXT_STREAM_S *pTencStream = (TEXT_STREAM_S*)&XencStream;
            nStreamLen = pTencStream->mLen + pTencStream->mExtraLen;
        }
        else
        {
            aloge("fatal error! unknown T");
        }
_CopyStream:
        int nBufLeftLen = it->mnBufSize - it->mnValidLen;
        if(nBufLeftLen >= nStreamLen)
        {
            it->CopyToStreamBuf(XencStream);
        }
        else
        {
            int nCurDuration = it->GetDuration();
            if(nCurDuration <= mCacheTime)
            {
                alogw("Be careful! streamId[%d] cache duration:[%d<=%d]ms, need increase streamBuf!", it->mStreamId, nCurDuration, mCacheTime);
            }
            //try to release oldest stream.
            MuxStreamNode *pFirstNode = it->PrefetchFirstStreamNode();
            if(0 == pFirstNode->mRefCnt)
            {
                alogw("streamId[%d] release ready stream node.", it->mStreamId);
                it->ReleaseReadyStreamNode(pFirstNode->GetStreamNodeId());
                goto _CopyStream;
            }

            //1. wait buf left space is enough.
            mbWaitReleasePacketFlag = true;
            bool bSuccess = false;
            int nTimeout = 2*1000; //unit:ms
            auto stop_waiting = [&]
            {
                bool bStop = false;
                nBufLeftLen = it->mnBufSize - it->mnValidLen;
                if(nBufLeftLen >= nStreamLen)
                {
                    bStop = true;
                }
                if(false == bStop)
                {
                    //alogw("Be careful! spurious wakeup! continue");
                }
                return bStop;
            };
            while(1)
            {
                bSuccess = mcvReleaseUsingPacket.wait_for(autoLock, std::chrono::milliseconds(nTimeout), stop_waiting);
                if(bSuccess)
                {
                    nBufLeftLen = it->mnBufSize - it->mnValidLen;
                    if(nBufLeftLen >= nStreamLen)
                    {
                        break;
                    }
                    else
                    {
                        aloge("fatal error! why streamId[%d] buf: [%d < %d]?", nStreamId, nBufLeftLen, nStreamLen);
                    }
                }
                else
                {
                    alogw("Be careful! streamId[%d] buf: wait free memory timeout[%d]ms", nStreamId, nTimeout);
                }
            }
            mbWaitReleasePacketFlag = false;
            //2. copy stream
            it->CopyToStreamBuf(XencStream);
        }
        return NO_ERROR;
    }
    template<class T> status_t ReleaseStream(const T& XencStream, int nStreamId)
    {
        status_t ret;
        std::lock_guard<std::mutex> autoLock(mLock);
        std::list<StreamBufManager>::iterator it;
        for(it=mStreamBufManagerList.begin(); it!=mStreamBufManagerList.end(); ++it)
        {
            if(it->mStreamId == nStreamId)
            {
                break;
            }
        }
        if(it == mStreamBufManagerList.end())
        {
            aloge("fatal error! invalid streamId[%d]", nStreamId);
            return UNKNOWN_ERROR;
        }
        int nNodeId = -1;
        if(std::is_same<T, VENC_STREAM_S>::value)
        {
            VENC_STREAM_S *pVencStream = (VENC_STREAM_S*)&XencStream;
            nNodeId = pVencStream->mSeq;
        }
        else if(std::is_same<T, AUDIO_STREAM_S>::value)
        {
            AUDIO_STREAM_S *pAencStream = (AUDIO_STREAM_S*)&XencStream;
            nNodeId = pAencStream->mId;
        }
        else if(std::is_same<T, TEXT_STREAM_S>::value)
        {
            TEXT_STREAM_S *pTencStream = (TEXT_STREAM_S*)&XencStream;
            nNodeId = pTencStream->mId;
        }
        else
        {
            aloge("fatal error! unknown T"); //typeid(XencStream).name()
        }
        ret = it->ReleaseStreamNode(nNodeId);
        if(mbWaitReleasePacketFlag)
        {
            mcvReleaseUsingPacket.notify_one();
        }
        return ret;
    }
    status_t ControlCacheLevel();
    MuxCacheManager(int nCacheTime, EyeseeRecorder *pRecorder);
    ~MuxCacheManager();
private:
    int mCacheTime; //unit:ms, indicate cache duration.
    EyeseeRecorder *mpOwner;
};

}
#endif  /* _MUXCACHEMANAGER_H_ */

