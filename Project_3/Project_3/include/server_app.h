#ifndef SERVER_APP_H
#define SERVER_APP_H

#include "auth.h"
#include "logger.h"
#include "stats.h"
#include "threadpool.h"
#include "tls.h"
#include "version_manager.h"
#include <pthread.h>
#include <signal.h>

typedef struct {
    int port;
    int max_workers;
    int accept_backlog;
    int queue_capacity;
    int chunk_size;
    int packet_delay_us;
    int client_timeout_sec;
    int pause_hold_timeout_sec;
    int enable_gui;
    int enable_tls;
    char manifest_path[MAX_PATH_LEN];
    char package_dir[MAX_PATH_LEN];
    char log_file[MAX_PATH_LEN];
    char tls_cert[MAX_PATH_LEN];
    char tls_key[MAX_PATH_LEN];
    char ca_cert[MAX_PATH_LEN];
    char client_registry[MAX_PATH_LEN];
    log_level_t log_level;
} server_config_t;

typedef struct {
    server_config_t cfg;
    int listen_fd;
    SSL_CTX *ssl_ctx;
    version_manager_t vm;
    tpq_t queue;
    stats_t stats;
    volatile sig_atomic_t *running;
    volatile sig_atomic_t *reload_requested;
} server_context_t;

typedef struct {
    server_context_t *ctx;
    int worker_id;
} worker_arg_t;

int server_config_load(server_config_t *cfg, const char *path);
void *acceptor_thread(void *arg);
void *worker_thread(void *arg);
void server_request_shutdown(void);
void server_track_worker_fd(int worker_id, int fd);

#endif
