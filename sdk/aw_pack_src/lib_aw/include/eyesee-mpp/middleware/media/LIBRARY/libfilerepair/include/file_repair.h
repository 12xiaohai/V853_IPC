#ifdef __cplusplus
extern "C" {
#endif

#ifndef __MP4_REPAIR_H__
#define __MP4_REPAIR_H__

#include <mm_comm_demux.h>

int mp4_file_repair(char const *file_path, DEMUX_MEDIA_INFO_S *pMediaInfo);

#endif


#ifdef __cplusplus
}
#endif
