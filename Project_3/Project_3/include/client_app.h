#ifndef CLIENT_APP_H
#define CLIENT_APP_H

#include "common.h"
#include "gui.h"
#include "logger.h"
#include "tls.h"
#include <signal.h>

typedef struct {
    char server_host[MAX_VALUE_LEN];
    int server_port;
    char client_version_file[MAX_PATH_LEN];
    char client_registry[MAX_PATH_LEN];
    char client_data_dir[MAX_PATH_LEN];
    char current_version[MAX_VERSION_STR];
    char download_dir[MAX_PATH_LEN];
    int retry_max;
    int retry_backoff_ms;
    int timeout_sec;
    int pause_timeout_sec;
    char log_file[MAX_PATH_LEN];
    int enable_gui;
    int enable_tls;
    char server_ca[MAX_PATH_LEN];
    char server_tls_name[MAX_VALUE_LEN];
    char client_cert[MAX_PATH_LEN];
    char client_key[MAX_PATH_LEN];
    char client_id[MAX_ID_LEN];
    log_level_t log_level;
} client_config_t;

typedef struct {
    client_config_t cfg;
    client_gui_state_t gui;
    SSL_CTX *ssl_ctx;
    volatile sig_atomic_t *stop_requested;
    int paused;
    int resume_retry_active;
} client_context_t;

int client_config_load(client_config_t *cfg, const char *path);
int getCurrentVersion(const client_config_t *cfg,
                      char out_version[MAX_VERSION_STR]);
int CheckForUpdate(client_context_t *ctx);
void client_errorf(const char *fmt, ...);
void client_set_active_fd(int fd);
int client_active_fd_was_signal_closed(int fd);
void client_request_stop(void);

#endif
