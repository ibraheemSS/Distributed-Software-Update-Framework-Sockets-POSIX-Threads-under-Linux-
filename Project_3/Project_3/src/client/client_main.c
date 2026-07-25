#include "client_app.h"
#include "auth.h"
#include "config.h"
#include "logger.h"
#include "tls.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static volatile sig_atomic_t g_client_stop = 0;
static volatile sig_atomic_t g_active_fd = -1;
static volatile sig_atomic_t g_signal_closed_fd = -1;
static client_context_t *g_client_ctx = NULL;
static int g_logger_started = 0;
static int g_cleanup_done = 0;
static int g_client_lock_fd = -1;
static char g_client_lock_path[MAX_PATH_LEN * 2];
static int g_stderr_is_tty = -1;

void client_errorf(const char *fmt, ...)
{
    va_list ap;
    if (g_stderr_is_tty < 0) {
        g_stderr_is_tty = isatty(fileno(stderr));
    }
    if (g_stderr_is_tty) {
        fputs("\033[31m", stderr);
    }
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    if (g_stderr_is_tty) {
        fputs("\033[0m", stderr);
    }
    fputs("\n\n", stderr);
}

static void handle_client_signal(int signo)
{
    (void)signo;
    client_request_stop();
}

void client_request_stop(void)
{
    g_client_stop = 1;
    int fd = (int)g_active_fd;
    if (fd >= 0) {
        g_active_fd = -1;
        g_signal_closed_fd = fd;
        close(fd);
    }
}

void client_set_active_fd(int fd)
{
    g_active_fd = fd;
    if (fd >= 0) {
        g_signal_closed_fd = -1;
    }
}

int client_active_fd_was_signal_closed(int fd)
{
    if (fd >= 0 && g_signal_closed_fd == fd) {
        g_signal_closed_fd = -1;
        return 1;
    }
    if (fd >= 0 && g_active_fd == fd) {
        g_active_fd = -1;
    }
    return 0;
}

static void cleanup_client(void)
{
    if (g_cleanup_done) {
        return;
    }
    g_cleanup_done = 1;
    if (g_client_ctx) {
        tls_free_ctx(g_client_ctx->ssl_ctx);
        g_client_ctx->ssl_ctx = NULL;
    }
    if (g_logger_started) {
        logger_stop();
        g_logger_started = 0;
    }
    if (g_client_lock_fd >= 0) {
        struct flock fl;
        memset(&fl, 0, sizeof(fl));
        fl.l_type = F_UNLCK;
        fl.l_whence = SEEK_SET;
        fcntl(g_client_lock_fd, F_SETLK, &fl);
        close(g_client_lock_fd);
        g_client_lock_fd = -1;
        g_client_lock_path[0] = '\0';
    }
}

static int mkdir_p_client(const char *path)
{
    if (!path || path[0] == '\0') {
        return -1;
    }

    char tmp[MAX_PATH_LEN * 2];
    int n = snprintf(tmp, sizeof(tmp), "%s", path);
    if (n <= 0 || (size_t)n >= sizeof(tmp)) {
        return -1;
    }

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

static int acquire_client_instance_lock(const client_config_t *cfg)
{
    if (!cfg || cfg->client_data_dir[0] == '\0' || cfg->client_id[0] == '\0') {
        return -1;
    }

    char client_dir[MAX_PATH_LEN * 2];
    int n = snprintf(client_dir, sizeof(client_dir), "%s/%s",
                     cfg->client_data_dir, cfg->client_id);
    if (n <= 0 || (size_t)n >= sizeof(client_dir)) {
        client_errorf("client: client data directory path is too long");
        return -1;
    }
    if (mkdir_p_client(client_dir) != 0) {
        client_errorf("client: cannot create client data folder");
        return -1;
    }

    n = snprintf(g_client_lock_path, sizeof(g_client_lock_path), "%s/client.lock",
                 client_dir);
    if (n <= 0 || (size_t)n >= sizeof(g_client_lock_path)) {
        client_errorf("client: lock path is too long");
        return -1;
    }

    int fd = open(g_client_lock_path, O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        client_errorf("client: cannot open lock file");
        g_client_lock_path[0] = '\0';
        return -1;
    }

    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    if (fcntl(fd, F_SETLK, &fl) != 0) {
        if (errno == EACCES || errno == EAGAIN) {
            client_errorf("client: %s is already in use. Close the other client terminal or choose another ID",
                          cfg->client_id);
        } else {
            client_errorf("client: cannot lock client session");
        }
        close(fd);
        g_client_lock_path[0] = '\0';
        return -1;
    }

    if (ftruncate(fd, 0) == 0) {
        dprintf(fd, "pid=%ld\nclient_id=%s\n", (long)getpid(), cfg->client_id);
        fsync(fd);
    }
    g_client_lock_fd = fd;
    return 0;
}

static void trim_line(char *s)
{
    s[strcspn(s, "\r\n")] = '\0';
    char *start = s;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }
    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
}

static int parse_client_id_input(const char *input, char out[MAX_ID_LEN])
{
    char line[128];
    snprintf(line, sizeof(line), "%s", input ? input : "");
    trim_line(line);

    if (line[0] == '\0') {
        return -1;
    }

    for (const char *p = line; *p; p++) {
        if (!isdigit((unsigned char)*p)) {
            return -1;
        }
    }

    const char *significant = line;
    while (*significant == '0') {
        significant++;
    }
    if (*significant == '\0') {
        return -1;
    }

    const char prefix[] = "client_";
    size_t prefix_len = sizeof(prefix) - 1;
    size_t sig_len = strlen(significant);
    size_t padded_len = sig_len < 3 ? 3 : sig_len;
    if (prefix_len + padded_len >= MAX_ID_LEN) {
        return -1;
    }

    char number[MAX_ID_LEN];
    size_t pad = padded_len - sig_len;
    memset(number, '0', pad);
    memcpy(number + pad, significant, sig_len + 1);
    memcpy(out, prefix, prefix_len);
    memcpy(out + prefix_len, number, padded_len + 1);
    return 0;
}

static int read_client_id(char out[MAX_ID_LEN], const char *default_id,
                          const char *registry_path)
{
    int interactive = !default_id || default_id[0] == '\0';
    char line[128];

    for (;;) {
        if (interactive) {
            printf("Enter client ID number: ");
            fflush(stdout);
            if (!fgets(line, sizeof(line), stdin)) {
                client_errorf("client: input closed");
                return -1;
            }
        } else {
            snprintf(line, sizeof(line), "%s", default_id);
        }

        if (parse_client_id_input(line, out) != 0) {
            if (!interactive) {
                client_errorf("client: CLIENT_ID must be numeric");
                return -1;
            }
            client_errorf("client: enter a numeric client ID");
            continue;
        }

        auth_client_status_t status = auth_client_status(registry_path, out);
        if (status == AUTH_CLIENT_OK) {
            if (interactive) {
                printf("client: using %s\n", out);
            }
            return 0;
        }
        if (status == AUTH_CLIENT_INVALID_REGISTRY) {
            client_errorf("client: invalid client registry");
            return -1;
        }

        if (!interactive) {
            client_errorf("client: %s is not enabled", out);
            return -1;
        }

        client_errorf("client: %s is %s",
                      out, status == AUTH_CLIENT_DISABLED ? "disabled" : "not registered");
    }
}

int client_config_load(client_config_t *out, const char *path)
{
    cfg_t cfg;
    if (!out || cfg_load(&cfg, path) != 0) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    cfg_copy_str(&cfg, "SERVER_HOST", "127.0.0.1", out->server_host, sizeof(out->server_host));
    if (cfg_get_int_range(&cfg, "SERVER_PORT", 5500, 1, 65535, &out->server_port) != 0 ||
        cfg_get_int_range(&cfg, "RETRY_MAX", 5, 1, 30, &out->retry_max) != 0 ||
        cfg_get_int_range(&cfg, "RETRY_BACKOFF_MS", 500, 10, 60000,
                          &out->retry_backoff_ms) != 0 ||
        cfg_get_int_range(&cfg, "TIMEOUT_SEC", 30, 1, 600, &out->timeout_sec) != 0 ||
        cfg_get_int_range(&cfg, "PAUSE_TIMEOUT_SEC", 30, 1, 3600,
                          &out->pause_timeout_sec) != 0) {
        return -1;
    }
    cfg_copy_str(&cfg, "CLIENT_VERSION_FILE", "./config/client_versions.txt",
                 out->client_version_file, sizeof(out->client_version_file));
    cfg_copy_str(&cfg, "CLIENT_REGISTRY", "./config/clients.keys",
                 out->client_registry, sizeof(out->client_registry));
    cfg_copy_str(&cfg, "CLIENT_DATA_DIR", "./clients",
                 out->client_data_dir, sizeof(out->client_data_dir));
    cfg_copy_str(&cfg, "CURRENT_VERSION", "", out->current_version,
                 sizeof(out->current_version));
    cfg_copy_str(&cfg, "DOWNLOAD_DIR", "", out->download_dir,
                 sizeof(out->download_dir));
    cfg_copy_str(&cfg, "LOG_FILE", "", out->log_file, sizeof(out->log_file));
    out->enable_gui = 1;
    out->enable_tls = 1;
    cfg_copy_str(&cfg, "SERVER_CA", "./config/ca.crt", out->server_ca,
                 sizeof(out->server_ca));
    cfg_copy_str(&cfg, "SERVER_TLS_NAME", "update-server",
                 out->server_tls_name, sizeof(out->server_tls_name));
    char default_id[MAX_ID_LEN];
    cfg_copy_str(&cfg, "CLIENT_ID", "", default_id, sizeof(default_id));
    if (read_client_id(out->client_id, default_id, out->client_registry) != 0) {
        return -1;
    }

    cfg_copy_str(&cfg, "CLIENT_CERT", "", out->client_cert,
                 sizeof(out->client_cert));
    cfg_copy_str(&cfg, "CLIENT_KEY", "", out->client_key,
                 sizeof(out->client_key));
    if (out->client_cert[0] == '\0') {
        snprintf(out->client_cert, sizeof(out->client_cert),
                 "./config/clients/%s.crt", out->client_id);
    }
    if (out->client_key[0] == '\0') {
        snprintf(out->client_key, sizeof(out->client_key),
                 "./config/clients/%s.key", out->client_id);
    }
    if (access(out->server_ca, R_OK) != 0 ||
        access(out->client_cert, R_OK) != 0 ||
        access(out->client_key, R_OK) != 0) {
        client_errorf("client: missing TLS files for %s", out->client_id);
        return -1;
    }

    if (out->download_dir[0] == '\0') {
        int n = snprintf(out->download_dir, sizeof(out->download_dir),
                         "%s/%s/downloads", out->client_data_dir, out->client_id);
        if (n <= 0 || (size_t)n >= sizeof(out->download_dir)) {
            client_errorf("client: download path is too long");
            return -1;
        }
    }
    if (out->log_file[0] == '\0') {
        int n = snprintf(out->log_file, sizeof(out->log_file),
                         "%s/%s/client.log", out->client_data_dir, out->client_id);
        if (n <= 0 || (size_t)n >= sizeof(out->log_file)) {
            client_errorf("client: log path is too long");
            return -1;
        }
    }
    out->log_level = log_level_from_string(cfg_get(&cfg, "LOG_LEVEL", "INFO"));
    if (out->client_id[0] == '\0' ||
        out->client_cert[0] == '\0' ||
        out->client_key[0] == '\0') {
        client_errorf("client: client id, cert, and key are required");
        return -1;
    }
    return 0;
}

static int run_update(void *arg)
{
    return CheckForUpdate((client_context_t *)arg);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        client_errorf("usage: %s config/client.conf", argv[0]);
        return 2;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_client_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    client_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.stop_requested = &g_client_stop;
    g_client_ctx = &ctx;
    atexit(cleanup_client);

    if (client_config_load(&ctx.cfg, argv[1]) != 0) {
        return 1;
    }
    if (acquire_client_instance_lock(&ctx.cfg) != 0) {
        return 1;
    }
    if (logger_start(ctx.cfg.log_file, ctx.cfg.log_level) != 0) {
        client_errorf("client: logger failed");
        return 1;
    }
    g_logger_started = 1;
    client_gui_state_init(&ctx.gui);

    if (ctx.cfg.enable_tls) {
        ctx.ssl_ctx = tls_create_client_ctx(ctx.cfg.server_ca,
                                            ctx.cfg.client_cert,
                                            ctx.cfg.client_key);
        if (!ctx.ssl_ctx) {
            log_msg(LOG_ERROR, "client", "-",
                    "TLS client context failed ca=%s cert=%s key=%s",
                    ctx.cfg.server_ca, ctx.cfg.client_cert,
                    ctx.cfg.client_key);
            logger_stop();
            g_logger_started = 0;
            return 1;
        }
    }

    int rc = client_gui_run(&ctx.gui, argc, argv, run_update, &ctx);

    cleanup_client();
    if (ctx.paused) {
        return 130;
    }
    if (rc == 130) {
        return 130;
    }
    return rc == 0 ? 0 : 1;
}
