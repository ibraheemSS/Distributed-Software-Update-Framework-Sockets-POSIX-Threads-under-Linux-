#include "threadpool.h"
#include <stdlib.h>
#include <string.h>

int tpq_init(tpq_t *q, int capacity)
{
    if (!q || capacity <= 0) {
        return -1;
    }
    memset(q, 0, sizeof(*q));
    q->items = calloc((size_t)capacity, sizeof(tpq_item_t));
    if (!q->items) {
        return -1;
    }
    q->capacity = capacity;
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    return 0;
}

void tpq_destroy(tpq_t *q)
{
    if (!q) {
        return;
    }
    pthread_mutex_destroy(&q->lock);
    pthread_cond_destroy(&q->not_empty);
    free(q->items);
    memset(q, 0, sizeof(*q));
}

int tpq_push(tpq_t *q, const tpq_item_t *item)
{
    if (!q || !item) {
        return -1;
    }
    pthread_mutex_lock(&q->lock);
    if (q->shutdown || q->count >= q->capacity) {
        pthread_mutex_unlock(&q->lock);
        return -1;
    }
    q->items[q->tail] = *item;
    q->tail = (q->tail + 1) % q->capacity;
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
    return 0;
}

int tpq_pop(tpq_t *q, tpq_item_t *out)
{
    if (!q || !out) {
        return -1;
    }
    pthread_mutex_lock(&q->lock);
    while (!q->shutdown && q->count == 0) {
        pthread_cond_wait(&q->not_empty, &q->lock);
    }
    if (q->count == 0 && q->shutdown) {
        pthread_mutex_unlock(&q->lock);
        return -1;
    }
    *out = q->items[q->head];
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    pthread_mutex_unlock(&q->lock);
    return 0;
}

void tpq_shutdown(tpq_t *q)
{
    if (!q) {
        return;
    }
    pthread_mutex_lock(&q->lock);
    q->shutdown = 1;
    pthread_cond_broadcast(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
}
