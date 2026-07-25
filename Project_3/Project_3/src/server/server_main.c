#include "server_app.h"
#include "config.h"
#include "gui.h"
#include "logger.h"
#include "netutil.h"
#include "tls.h"
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#define TRACKED_WORKER_FDS 257

static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_reload_requested = 0;
static int g_listen_fd = -1;
static volatile sig_atomic_t g_worker_fds[TRACKED_WORKER_FDS] =
    { [0 ... TRACKED_WORKER_FDS - 1] = -1 };
static server_context_t *g_server_ctx = NULL;
static pthread_t g_acceptor_thread;
static pthread_t g_reloader_thread;
static pthread_t *g_worker_threads = NULL;
static worker_arg_t *g_worker_args = NULL;
static pthread_t g_shutdown_watcher_thread;
static int g_acceptor_started = 0;
static int g_reloader_started = 0;
static int g_shutdown_watcher_started = 0;
static int g_workers_started = 0;
static int g_queue_ready = 0;
static int g_vm_ready = 0;
static int g_logger_started = 0;
static int g_cleanup_done = 0;
static pthread_mutex_t g_shutdown_lock = PTHREAD_MUTEX_INITIALIZER;

static void server_errorf(const char *fmt, ...)
{
    static int stderr_is_tty = -1;
    va_list ap;

    if (stderr_is_tty < 0) {
        stderr_is_tty = isatty(fileno(stderr));
    }
    if (stderr_is_tty) {
        fputs("\033[31m", stderr);
    }
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    if (stderr_is_tty) {
        fputs("\033[0m", stderr);
    }
    fputs("\n\n", stderr);
}

static void shutdown_active_worker_fds(void)
{
    for (int i = 0; i < TRACKED_WORKER_FDS; i++) {
        int fd = (int)g_worker_fds[i];
        if (fd >= 0) {
            g_worker_fds[i] = -1;
            shutdown(fd, SHUT_RDWR);
        }
    }
}

void server_track_worker_fd(int worker_id, int fd)
{
    if (worker_id <= 0 || worker_id >= TRACKED_WORKER_FDS) {
        return;
    }
    g_worker_fds[worker_id] = fd;
}

void server_request_shutdown(void)
{
    pthread_mutex_lock(&g_shutdown_lock);
    g_running = 0;
    if (g_listen_fd >= 0) {
        int fd = g_listen_fd;
        g_listen_fd = -1;
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }
    shutdown_active_worker_fds();
    if (g_server_ctx && g_queue_ready) {
        tpq_shutdown(&g_server_ctx->queue);
    }
    pthread_mutex_unlock(&g_shutdown_lock);
}

static void handle_signal(int signo)
{
    if (signo == SIGHUP) {
        g_reload_requested = 1;
        return;
    }
    g_running = 0;
}

static void *shutdown_watcher_thread(void *arg)
{
    (void)arg;
    while (g_running) {
        usleep(100000);
    }
    server_request_shutdown();
    return NULL;
}

static void cleanup_server(void)
{
    if (g_cleanup_done) {
        return;
    }
    g_cleanup_done = 1;
    server_request_shutdown();

    if (g_shutdown_watcher_started &&
        !pthread_equal(pthread_self(), g_shutdown_watcher_thread)) {
        pthread_join(g_shutdown_watcher_thread, NULL);
        g_shutdown_watcher_started = 0;
    }
    if (g_acceptor_started) {
        pthread_join(g_acceptor_thread, NULL);
        g_acceptor_started = 0;
    }
    if (g_reloader_started) {
        pthread_join(g_reloader_thread, NULL);
        g_reloader_started = 0;
    }
    for (int i = 0; i < g_workers_started; i++) {
        pthread_join(g_worker_threads[i], NULL);
    }
    g_workers_started = 0;

    if (g_server_ctx) {
        tls_free_ctx(g_server_ctx->ssl_ctx);
        g_server_ctx->ssl_ctx = NULL;
        if (g_queue_ready) {
            tpq_destroy(&g_server_ctx->queue);
            g_queue_ready = 0;
        }
        if (g_vm_ready) {
            vm_destroy(&g_server_ctx->vm);
            g_vm_ready = 0;
        }
    }

    free(g_worker_threads);
    free(g_worker_args);
    g_worker_threads = NULL;
    g_worker_args = NULL;

    if (g_logger_started) {
        log_msg(LOG_INFO, "main", "-", "SERVER_SHUTDOWN");
        logger_stop();
        g_logger_started = 0;
    }
}

int server_config_load(server_config_t *out, const char *path)
{
    cfg_t cfg;
    if (!out || cfg_load(&cfg, path) != 0) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->packet_delay_us = DEFAULT_PACKET_DELAY_US;
    if (cfg_get_int_range(&cfg, "PORT", 5500, 1024, 65535, &out->port) != 0 ||
        cfg_get_int_range(&cfg, "MAX_WORKERS", 16, 1, 256, &out->max_workers) != 0 ||
        cfg_get_int_range(&cfg, "ACCEPT_BACKLOG", 64, 1, 1024,
                          &out->accept_backlog) != 0 ||
        cfg_get_int_range(&cfg, "QUEUE_CAPACITY", 128, 1, 4096,
                          &out->queue_capacity) != 0 ||
        cfg_get_int_range(&cfg, "CHUNK_SIZE", DEFAULT_CHUNK_SIZE,
                          MIN_CHUNK_SIZE, MAX_CHUNK_SIZE, &out->chunk_size) != 0 ||
        cfg_get_int_range(&cfg, "CLIENT_TIMEOUT_SEC", 30, 1, 600,
                          &out->client_timeout_sec) != 0 ||
        cfg_get_int_range(&cfg, "PAUSE_HOLD_TIMEOUT_SEC", 5, 1, 600,
                          &out->pause_hold_timeout_sec) != 0) {
        return -1;
    }
    out->enable_gui = 1;
    out->enable_tls = 1;
    cfg_copy_str(&cfg, "MANIFEST_PATH", "./config/manifest.conf",
                 out->manifest_path, sizeof(out->manifest_path));
    cfg_copy_str(&cfg, "PACKAGE_DIR", "./packages",
                 out->package_dir, sizeof(out->package_dir));
    cfg_copy_str(&cfg, "LOG_FILE", "./logs/server.log",
                 out->log_file, sizeof(out->log_file));
    cfg_copy_str(&cfg, "TLS_CERT", "./config/server.crt",
                 out->tls_cert, sizeof(out->tls_cert));
    cfg_copy_str(&cfg, "TLS_KEY", "./config/server.key",
                 out->tls_key, sizeof(out->tls_key));
    cfg_copy_str(&cfg, "CA_CERT", "./config/ca.crt",
                 out->ca_cert, sizeof(out->ca_cert));
    cfg_copy_str(&cfg, "CLIENT_REGISTRY", "./config/clients.keys",
                 out->client_registry, sizeof(out->client_registry));
    out->log_level = log_level_from_string(cfg_get(&cfg, "LOG_LEVEL", "INFO"));
    return 0;
}

static void *reload_thread(void *arg)
{
    server_context_t *ctx = (server_context_t *)arg;
    while (*(ctx->running)) {
        if (*(ctx->reload_requested)) {
            *(ctx->reload_requested) = 0;
            vm_reload(&ctx->vm);
        }
        sleep(1);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        server_errorf("usage: %s config/server.conf", argv[0]);
        return 2;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    server_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.listen_fd = -1;
    ctx.running = &g_running;
    ctx.reload_requested = &g_reload_requested;
    g_server_ctx = &ctx;
    atexit(cleanup_server);

    if (server_config_load(&ctx.cfg, argv[1]) != 0) {
        server_errorf("server: cannot load config %s", argv[1]);
        return 1;
    }
    if (logger_start(ctx.cfg.log_file, ctx.cfg.log_level) != 0) {
        server_errorf("server: logger failed");
        return 1;
    }
    g_logger_started = 1;
    log_msg(LOG_INFO, "main", "-", "SERVER_START port=%d workers=%d tls=%d",
            ctx.cfg.port, ctx.cfg.max_workers, ctx.cfg.enable_tls);

    if (vm_init(&ctx.vm, ctx.cfg.manifest_path, ctx.cfg.package_dir) != 0) {
        logger_stop();
        g_logger_started = 0;
        return 1;
    }
    g_vm_ready = 1;
    if (tpq_init(&ctx.queue, ctx.cfg.queue_capacity) != 0) {
        log_msg(LOG_ERROR, "main", "-", "QUEUE init failed");
        cleanup_server();
        return 1;
    }
    g_queue_ready = 1;
    stats_init(&ctx.stats, ctx.cfg.max_workers);

    if (ctx.cfg.enable_tls) {
        ctx.ssl_ctx = tls_create_server_ctx(ctx.cfg.tls_cert, ctx.cfg.tls_key,
                                            ctx.cfg.ca_cert);
        if (!ctx.ssl_ctx) {
            log_msg(LOG_ERROR, "main", "-",
                    "TLS context failed cert=%s key=%s ca=%s",
                    ctx.cfg.tls_cert, ctx.cfg.tls_key, ctx.cfg.ca_cert);
            cleanup_server();
            return 1;
        }
    }

    ctx.listen_fd = net_create_listener(ctx.cfg.port, ctx.cfg.accept_backlog);
    if (ctx.listen_fd < 0) {
        if (errno == EADDRINUSE) {
            server_errorf("server: port %d is already in use. Another server is running",
                          ctx.cfg.port);
        } else if (errno == EACCES) {
            server_errorf("server: permission denied on port %d", ctx.cfg.port);
        } else {
            server_errorf("server: cannot start on port %d: %s",
                          ctx.cfg.port, strerror(errno));
        }
        cleanup_server();
        return 1;
    }
    g_listen_fd = ctx.listen_fd;

    if (pthread_create(&g_shutdown_watcher_thread, NULL,
                       shutdown_watcher_thread, NULL) != 0) {
        log_msg(LOG_ERROR, "main", "-", "SHUTDOWN_WATCHER_CREATE failed");
        g_running = 0;
    } else {
        g_shutdown_watcher_started = 1;
    }

    g_worker_threads = calloc((size_t)ctx.cfg.max_workers, sizeof(pthread_t));
    g_worker_args = calloc((size_t)ctx.cfg.max_workers, sizeof(worker_arg_t));
    if (!g_worker_threads || !g_worker_args) {
        log_msg(LOG_ERROR, "main", "-", "THREAD allocation failed");
        cleanup_server();
        return 1;
    }

    for (int i = 0; g_running && i < ctx.cfg.max_workers; i++) {
        g_worker_args[i].ctx = &ctx;
        g_worker_args[i].worker_id = i + 1;
        if (pthread_create(&g_worker_threads[i], NULL, worker_thread, &g_worker_args[i]) != 0) {
            log_msg(LOG_ERROR, "main", "-", "WORKER_CREATE failed index=%d", i);
            g_running = 0;
            break;
        }
        g_workers_started++;
    }
    if (g_running && pthread_create(&g_acceptor_thread, NULL, acceptor_thread, &ctx) != 0) {
        log_msg(LOG_ERROR, "main", "-", "ACCEPTOR_CREATE failed");
        g_running = 0;
    } else if (g_running) {
        g_acceptor_started = 1;
    }
    if (g_running && pthread_create(&g_reloader_thread, NULL, reload_thread, &ctx) != 0) {
        log_msg(LOG_ERROR, "main", "-", "RELOAD_THREAD_CREATE failed");
        g_running = 0;
    } else if (g_running) {
        g_reloader_started = 1;
    }

    if (g_running) {
        server_dashboard_run(&ctx.stats, &g_running, argc, argv);
    }

    cleanup_server();
    return 0;
}
