#include "log.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <utils/plat_log.h>

#define LOG_DIRECTORY "/tmp/log"

/* 配置 glog：INFO 及以上日志写入 /tmp/log，并将 INFO 同时显示在终端。 */
int init_glog(char *argv[])
{
    GLogConfig config = {
        .FLAGS_logtostderr = 0,
        .FLAGS_colorlogtostderr = 1,
        .FLAGS_stderrthreshold = _GLOG_INFO,
        .FLAGS_minloglevel = _GLOG_INFO,
        .FLAGS_logbuflevel = -1,
        .FLAGS_logbufsecs = 0,
        .FLAGS_max_log_size = 1,
        .FLAGS_stop_logging_if_full_disk = 1,
    };

    if (argv == NULL || argv[0] == NULL) {
        return -1;
    }

    /* mkdir 遇到 EEXIST 代表目录已存在，不是错误。 */
    if (mkdir(LOG_DIRECTORY, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "cannot create log directory %s: %s\n",
                LOG_DIRECTORY, strerror(errno));
        return -1;
    }

    /* 最终文件名由 glog 按目录、前缀和扩展信息组合。 */
    strcpy(config.LogDir, LOG_DIRECTORY);
    strcpy(config.InfoLogFileNameBase, "LOG-");
    strcpy(config.LogFileNameExtension, "IPC-");
    log_init(argv[0], &config);
    return 0;
}

void deinit_glog(void)
{
    /* 在程序退出前刷新并释放 glog 内部资源。 */
    log_quit();
}
