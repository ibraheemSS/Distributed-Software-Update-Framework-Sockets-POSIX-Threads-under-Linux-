#include "stats.h"
#include <stdio.h>
#include <string.h>

void stats_init(stats_t *s, int max_workers)
{
    atomic_init(&s->connections, 0);
    atomic_init(&s->active_transfers, 0);
    atomic_init(&s->updates_served, 0);
    atomic_init(&s->uptodate_responses, 0);
    atomic_init(&s->auth_failures, 0);
    atomic_init(&s->errors, 0);
    atomic_init(&s->bytes_sent, 0);
    atomic_init(&s->busy_workers, 0);
    s->max_workers = max_workers;
    pthread_mutex_init(&s->active_lock, NULL);
    s->active_client_count = 0;
}

static int find_active_key(stats_t *s, const char *key)
{
    for (int i = 0; i < s->active_client_count; i++) {
        if (strcmp(s->active_client_keys[i], key) == 0) {
            return i;
        }
    }
    return -1;
}

void stats_active_client_add(stats_t *s, const char *key, const char *label)
{
    if (!s || !key || key[0] == '\0') {
        return;
    }

    pthread_mutex_lock(&s->active_lock);
    int idx = find_active_key(s, key);
    if (idx >= 0) {
        snprintf(s->active_client_labels[idx], ACTIVE_CLIENT_LABEL, "%s",
                 label && label[0] ? label : key);
        pthread_mutex_unlock(&s->active_lock);
        return;
    }

    if (s->active_client_count < ACTIVE_CLIENT_MAX) {
        idx = s->active_client_count++;
        snprintf(s->active_client_keys[idx], CLIENT_INFO_LEN, "%s", key);
        snprintf(s->active_client_labels[idx], ACTIVE_CLIENT_LABEL, "%s",
                 label && label[0] ? label : key);
    }
    pthread_mutex_unlock(&s->active_lock);
}

void stats_active_client_update(stats_t *s, const char *key, const char *label)
{
    if (!s || !key || key[0] == '\0') {
        return;
    }

    pthread_mutex_lock(&s->active_lock);
    int idx = find_active_key(s, key);
    if (idx >= 0) {
        snprintf(s->active_client_labels[idx], ACTIVE_CLIENT_LABEL, "%s",
                 label && label[0] ? label : key);
    }
    pthread_mutex_unlock(&s->active_lock);
}

void stats_active_client_remove(stats_t *s, const char *key)
{
    if (!s || !key || key[0] == '\0') {
        return;
    }

    pthread_mutex_lock(&s->active_lock);
    int idx = find_active_key(s, key);
    if (idx >= 0) {
        for (int i = idx + 1; i < s->active_client_count; i++) {
            snprintf(s->active_client_keys[i - 1], CLIENT_INFO_LEN, "%s",
                     s->active_client_keys[i]);
            snprintf(s->active_client_labels[i - 1], ACTIVE_CLIENT_LABEL, "%s",
                     s->active_client_labels[i]);
        }
        s->active_client_count--;
        s->active_client_keys[s->active_client_count][0] = '\0';
        s->active_client_labels[s->active_client_count][0] = '\0';
    }
    pthread_mutex_unlock(&s->active_lock);
}

int stats_active_client_snapshot(stats_t *s,
                                 char labels[][ACTIVE_CLIENT_LABEL],
                                 int max_labels)
{
    if (!s || !labels || max_labels <= 0) {
        return 0;
    }

    pthread_mutex_lock(&s->active_lock);
    int total = s->active_client_count;
    int n = total < max_labels ? total : max_labels;
    for (int i = 0; i < n; i++) {
        snprintf(labels[i], ACTIVE_CLIENT_LABEL, "%s", s->active_client_labels[i]);
    }
    pthread_mutex_unlock(&s->active_lock);
    return total;
}
