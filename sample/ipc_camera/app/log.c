#include <utils/plat_log.h>
#include <string.h>

void init_glog(char *argv[]) {

  GLogConfig stGLogConfig = {
      .FLAGS_logtostderr = 0,
      .FLAGS_colorlogtostderr = 1,
      .FLAGS_stderrthreshold = _GLOG_INFO,
      .FLAGS_minloglevel = _GLOG_INFO,
      .FLAGS_logbuflevel = -1,
      .FLAGS_logbufsecs = 0,
      .FLAGS_max_log_size = 1,
      .FLAGS_stop_logging_if_full_disk = 1,
  };
  strcpy(stGLogConfig.LogDir, "/tmp/log");
  strcpy(stGLogConfig.InfoLogFileNameBase, "LOG-");
  strcpy(stGLogConfig.LogFileNameExtension, "IPC-");
  log_init(argv[0], &stGLogConfig);
}
