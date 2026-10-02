#ifndef VIPP4_TEST_LOG_H
#define VIPP4_TEST_LOG_H
void vipp4_test_log(const char *format, ...);
#define alogd(...) vipp4_test_log(__VA_ARGS__)
#define alogw(...) vipp4_test_log(__VA_ARGS__)
#define aloge(...) vipp4_test_log(__VA_ARGS__)
#endif
