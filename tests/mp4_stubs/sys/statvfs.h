#ifndef MP4_TEST_STATVFS_H
#define MP4_TEST_STATVFS_H
/* Windows主机没有statvfs；仅测试替身，不进入板端构建。 */
#ifdef _WIN32
struct statvfs { unsigned long long f_bavail, f_frsize; };
static inline int statvfs(const char *path, struct statvfs *value)
{
    (void)path;
    value->f_bavail = 1024U * 1024U;
    value->f_frsize = 4096U;
    return 0;
}
#else
#include_next <sys/statvfs.h>
#endif
#endif
