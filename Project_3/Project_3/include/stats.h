#ifndef STATS_H
#define STATS_H

#include "common.h"
#include <pthread.h>
#include <stdatomic.h>

#define ACTIVE_CLIENT_MAX 128
#define ACTIVE_CLIENT_LABEL 160

typedef struct {
    atomic_ulong connections;
    atomic_ulong active_transfers;
    atomic_ulong updates_served;
    atomic_ulong uptodate_responses;
    atomic_ulong auth_failures;
    atomic_ulong errors;
    atomic_ullong bytes_sent;
    atomic_int busy_workers;
    int max_workers;
    pthread_mutex_t active_lock;
    char active_client_keys[ACTIVE_CLIENT_MAX][CLIENT_INFO_LEN];
    char active_client_labels[ACTIVE_CLIENT_MAX][ACTIVE_CLIENT_LABEL];
    int active_client_count;
} stats_t;

void stats_init(stats_t *s, int max_workers);
void stats_active_client_add(stats_t *s, const char *key, const char *label);
void stats_active_client_update(stats_t *s, const char *key, const char *label);
void stats_active_client_remove(stats_t *s, const char *key);
int stats_active_client_snapshot(stats_t *s,
                                 char labels[][ACTIVE_CLIENT_LABEL],
                                 int max_labels);

#endif
