#ifndef _MUXSTREAMNODE_H_
#define _MUXSTREAMNODE_H_

#include <mm_comm_venc.h>
#include <mm_comm_aio.h>
#include <mm_comm_tenc.h>

namespace EyeseeLinux {

struct MuxStreamNode
{
    int mStreamId;
    enum class Type
    {
        Video = 0,
        Audio = 1,
        Text = 2,
        Unknown = 3,
    };
    Type mStreamType;
    union
    {
        VENC_STREAM_S mVencStream;
        AUDIO_STREAM_S mAencStream;
        TEXT_STREAM_S mTencStream;
    };
    int mRefCnt;
    int GetStreamLen();
    int64_t GetStreamPts();
    bool CheckKeyFrameFlag(PAYLOAD_TYPE_E ePayloadType);
    int GetStreamNodeId();
    MuxStreamNode(const VENC_STREAM_S& VEncStream, int nStreamId);
    MuxStreamNode(const AUDIO_STREAM_S& AEncStream, int nStreamId);
    MuxStreamNode(const TEXT_STREAM_S& TEncStream, int nStreamId);
    MuxStreamNode(const MuxStreamNode& lRef);
    ~MuxStreamNode();
};

}
#endif  /* _MUXSTREAMNODE_H_ */

