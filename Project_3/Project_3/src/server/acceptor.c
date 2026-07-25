#include "server_app.h"
#include "logger.h"
#include "netutil.h"
#include <errno.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

void *acceptor_thread(void *arg)
{
    server_context_t *ctx = (server_context_t *)arg;
    log_msg(LOG_INFO, "acceptor", "-", "ACCEPTOR_START listen_fd=%d", ctx->listen_fd);

    while (*(ctx->running)) {
        int listen_fd = ctx->listen_fd;
        if (listen_fd < 0) {
            break;
        }
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200000;

        int ready = select(listen_fd + 1, &rfds, NULL, NULL, &tv);
        if (ready < 0) {
            if (!*(ctx->running) || errno == EBADF || errno == EINVAL) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            log_msg(LOG_WARN, "acceptor", "-", "SELECT failed errno=%d", errno);
            continue;
        }
        if (ready == 0) {
            continue;
        }

        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) {
            if (!*(ctx->running) || errno == EBADF || errno == EINVAL) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            log_msg(LOG_WARN, "acceptor", "-", "ACCEPT failed errno=%d", errno);
            continue;
        }

        net_set_timeouts(fd, ctx->cfg.client_timeout_sec);
        tpq_item_t item;
        memset(&item, 0, sizeof(item));
        item.fd = fd;
        net_peer_info(fd, item.client_info, sizeof(item.client_info));
        atomic_fetch_add(&ctx->stats.connections, 1);

        if (tpq_push(&ctx->queue, &item) != 0) {
            conn_send_plain_busy(fd);
            log_msg(LOG_WARN, "acceptor", item.client_info, "REJECT busy queue_full=1");
            close(fd);
            continue;
        }
        log_msg(LOG_INFO, "acceptor", item.client_info, "ACCEPT queued");
    }

    log_msg(LOG_INFO, "acceptor", "-", "ACCEPTOR_STOP");
    return NULL;
}
