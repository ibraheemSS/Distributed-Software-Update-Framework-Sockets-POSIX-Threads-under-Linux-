#ifndef THREADPOOL_H
#define THREADPOOL_H

#include "common.h"
#include <pthread.h>

typedef struct {
    int fd;
    char client_info[CLIENT_INFO_LEN];
} tpq_item_t;

typedef struct {
    tpq_item_t *items;
    int capacity;
    int head;
    int tail;
    int count;
    int shutdown;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
} tpq_t;

int tpq_init(tpq_t *q, int capacity);
void tpq_destroy(tpq_t *q);
int tpq_push(tpq_t *q, const tpq_item_t *item);
int tpq_pop(tpq_t *q, tpq_item_t *out);
void tpq_shutdown(tpq_t *q);

#endif
