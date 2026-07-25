#ifndef LOGGER_H
#define LOGGER_H

#include "common.h"

typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO  = 1,
    LOG_WARN  = 2,
    LOG_ERROR = 3
} log_level_t;

int logger_start(const char *path, log_level_t min_level);
void logger_stop(void);
void log_msg(log_level_t lvl, const char *thread_tag,
             const char *client_info, const char *fmt, ...);
int logger_recent(char dst[][LOG_LINE_MAX], int max);
log_level_t log_level_from_string(const char *s);

#endif
