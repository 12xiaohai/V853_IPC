#ifndef REGION_TEST_PLAT_LOG_H
#define REGION_TEST_PLAT_LOG_H

#include <stdarg.h>
#include <stdio.h>

/* 主机测试替身：保留格式检查和日志输出，不链接厂商glog/MPP。 */
static inline void region_test_log(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
    fputc('\n', stderr);
}
#define alogd(...) region_test_log(__VA_ARGS__)
#define alogw(...) region_test_log(__VA_ARGS__)
#define aloge(...) region_test_log(__VA_ARGS__)

#endif
