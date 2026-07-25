#include "logger.h"
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

typedef struct {
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    char queue[LOG_QUEUE_CAP][LOG_LINE_MAX];
    int head;
    int tail;
    int count;
    int stop;
    unsigned long dropped;

    pthread_mutex_t ring_lock;
    char ring[GUI_LOG_RING][LOG_LINE_MAX];
    int ring_start;
    int ring_count;

    FILE *fp;
    int owns_file;
    int started;
    log_level_t min_level;
} logger_state_t;

static logger_state_t g_logger;

static const char *level_text(log_level_t lvl)
{
    switch (lvl) {
    case LOG_DEBUG: return "DEBUG";
    case LOG_INFO:  return "INFO ";
    case LOG_WARN:  return "WARN ";
    case LOG_ERROR: return "ERROR";
    default:        return "INFO ";
    }
}

log_level_t log_level_from_string(const char *s)
{
    if (!s) {
        return LOG_INFO;
    }
    if (strcasecmp(s, "DEBUG") == 0) {
        return LOG_DEBUG;
    }
    if (strcasecmp(s, "WARN") == 0 || strcasecmp(s, "WARNING") == 0) {
        return LOG_WARN;
    }
    if (strcasecmp(s, "ERROR") == 0) {
        return LOG_ERROR;
    }
    return LOG_INFO;
}

static void make_parent_dirs(const char *path)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
}

static void ring_add(const char *line)
{
    pthread_mutex_lock(&g_logger.ring_lock);
    int idx = (g_logger.ring_start + g_logger.ring_count) % GUI_LOG_RING;
    if (g_logger.ring_count == GUI_LOG_RING) {
        idx = g_logger.ring_start;
        g_logger.ring_start = (g_logger.ring_start + 1) % GUI_LOG_RING;
    } else {
        g_logger.ring_count++;
    }
    snprintf(g_logger.ring[idx], LOG_LINE_MAX, "%s", line);
    pthread_mutex_unlock(&g_logger.ring_lock);
}

static void write_line_unlocked(const char *line)
{
    FILE *fp = g_logger.fp ? g_logger.fp : stderr;
    fprintf(fp, "%s\n", line);
    fflush(fp);
    ring_add(line);
}

static void *logger_thread(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_logger.lock);
        while (!g_logger.stop && g_logger.count == 0) {
            pthread_cond_wait(&g_logger.not_empty, &g_logger.lock);
        }
        if (g_logger.stop && g_logger.count == 0) {
            pthread_mutex_unlock(&g_logger.lock);
            break;
        }

        char line[LOG_LINE_MAX];
        snprintf(line, sizeof(line), "%s", g_logger.queue[g_logger.head]);
        g_logger.head = (g_logger.head + 1) % LOG_QUEUE_CAP;
        g_logger.count--;
        unsigned long dropped = g_logger.dropped;
        g_logger.dropped = 0;
        pthread_mutex_unlock(&g_logger.lock);

        if (dropped > 0) {
            char warn[LOG_LINE_MAX];
            snprintf(warn, sizeof(warn),
                     "logger queue overflow: dropped %lu log lines", dropped);
            write_line_unlocked(warn);
        }
        write_line_unlocked(line);
    }
    return NULL;
}

int logger_start(const char *path, log_level_t min_level)
{
    memset(&g_logger, 0, sizeof(g_logger));
    pthread_mutex_init(&g_logger.lock, NULL);
    pthread_cond_init(&g_logger.not_empty, NULL);
    pthread_mutex_init(&g_logger.ring_lock, NULL);
    g_logger.min_level = min_level;

    if (path && *path) {
        make_parent_dirs(path);
        g_logger.fp = fopen(path, "a");
        if (g_logger.fp) {
            g_logger.owns_file = 1;
        }
    }
    if (!g_logger.fp) {
        g_logger.fp = stderr;
        g_logger.owns_file = 0;
    }

    if (pthread_create(&g_logger.thread, NULL, logger_thread, NULL) != 0) {
        if (g_logger.owns_file) {
            fclose(g_logger.fp);
        }
        return -1;
    }
    g_logger.started = 1;
    return 0;
}

void logger_stop(void)
{
    if (!g_logger.started) {
        return;
    }
    pthread_mutex_lock(&g_logger.lock);
    g_logger.stop = 1;
    pthread_cond_signal(&g_logger.not_empty);
    pthread_mutex_unlock(&g_logger.lock);
    pthread_join(g_logger.thread, NULL);

    if (g_logger.owns_file && g_logger.fp) {
        fclose(g_logger.fp);
    }
    pthread_mutex_destroy(&g_logger.lock);
    pthread_cond_destroy(&g_logger.not_empty);
    pthread_mutex_destroy(&g_logger.ring_lock);
    memset(&g_logger, 0, sizeof(g_logger));
}

void log_msg(log_level_t lvl, const char *thread_tag,
             const char *client_info, const char *fmt, ...)
{
    if (lvl < g_logger.min_level) {
        return;
    }

    char message[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);

    char stamp[64];
    snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d %02d:%02d:%02d.%03ld",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000L);

    char line[LOG_LINE_MAX];
    snprintf(line, sizeof(line), "%s [%s] [%s] [%s] %s",
             stamp, level_text(lvl), thread_tag ? thread_tag : "-",
             client_info ? client_info : "-", message);

    if (!g_logger.started) {
        fprintf(stderr, "%s\n", line);
        return;
    }

    pthread_mutex_lock(&g_logger.lock);
    if (g_logger.count == LOG_QUEUE_CAP) {
        g_logger.dropped++;
        pthread_mutex_unlock(&g_logger.lock);
        return;
    }
    snprintf(g_logger.queue[g_logger.tail], LOG_LINE_MAX, "%s", line);
    g_logger.tail = (g_logger.tail + 1) % LOG_QUEUE_CAP;
    g_logger.count++;
    pthread_cond_signal(&g_logger.not_empty);
    pthread_mutex_unlock(&g_logger.lock);
}

int logger_recent(char dst[][LOG_LINE_MAX], int max)
{
    if (!dst || max <= 0) {
        return 0;
    }
    pthread_mutex_lock(&g_logger.ring_lock);
    int n = g_logger.ring_count < max ? g_logger.ring_count : max;
    int start = (g_logger.ring_start + g_logger.ring_count - n + GUI_LOG_RING) % GUI_LOG_RING;
    for (int i = 0; i < n; i++) {
        int idx = (start + i) % GUI_LOG_RING;
        snprintf(dst[i], LOG_LINE_MAX, "%s", g_logger.ring[idx]);
    }
    pthread_mutex_unlock(&g_logger.ring_lock);
    return n;
}
