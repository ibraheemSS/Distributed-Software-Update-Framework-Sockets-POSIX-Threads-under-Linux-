#include "server_app.h"
#include "logger.h"
#include "netutil.h"
#include "package_service.h"
#include "protocol.h"
#include "tls.h"
#include "version.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static pthread_mutex_t g_active_id_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_active_client_ids[ACTIVE_CLIENT_MAX][MAX_ID_LEN];

static int reserve_active_client_id(const char *client_id)
{
    if (!client_id || client_id[0] == '\0') {
        return -1;
    }

    int free_slot = -1;
    pthread_mutex_lock(&g_active_id_lock);
    for (int i = 0; i < ACTIVE_CLIENT_MAX; i++) {
        if (g_active_client_ids[i][0] == '\0') {
            if (free_slot < 0) {
                free_slot = i;
            }
            continue;
        }
        if (strcmp(g_active_client_ids[i], client_id) == 0) {
            pthread_mutex_unlock(&g_active_id_lock);
            return 1;
        }
    }

    if (free_slot < 0) {
        pthread_mutex_unlock(&g_active_id_lock);
        return -1;
    }
    snprintf(g_active_client_ids[free_slot], MAX_ID_LEN, "%s", client_id);
    pthread_mutex_unlock(&g_active_id_lock);
    return 0;
}

static void release_active_client_id(const char *client_id)
{
    if (!client_id || client_id[0] == '\0') {
        return;
    }

    pthread_mutex_lock(&g_active_id_lock);
    for (int i = 0; i < ACTIVE_CLIENT_MAX; i++) {
        if (strcmp(g_active_client_ids[i], client_id) == 0) {
            g_active_client_ids[i][0] = '\0';
            break;
        }
    }
    pthread_mutex_unlock(&g_active_id_lock);
}

static int recv_control(conn_t *conn, msg_type_t *type, char *payload, size_t payload_len,
                        const char *thread_tag, const char *client_info)
{
    uint8_t flags = 0;
    uint32_t len = 0;
    int rc = proto_recv(conn, type, &flags, payload, payload_len, &len, 0);
    if (rc != PROTO_RECV_OK) {
        log_msg(LOG_WARN, thread_tag, client_info, "PROTO_RECV failed rc=%d", rc);
        return -1;
    }
    (void)flags;
    payload[len] = '\0';
    return 0;
}

static int authenticate_client(server_context_t *ctx, conn_t *conn,
                               const char *thread_tag, const char *client_info,
                               char client_id[MAX_ID_LEN])
{
    char cert_id[MAX_ID_LEN] = "";
    if (!conn->ssl ||
        tls_peer_common_name(conn->ssl, cert_id, sizeof(cert_id)) != 0) {
        atomic_fetch_add(&ctx->stats.auth_failures, 1);
        log_msg(LOG_WARN, thread_tag, client_info,
                "AUTH fail missing TLS client certificate identity");
        proto_send_error(conn, "AUTH_FAIL", "missing client certificate");
        return -1;
    }

    char payload[MAX_CONTROL_PAYLOAD + 1];
    msg_type_t type;
    if (recv_control(conn, &type, payload, sizeof(payload), thread_tag, client_info) != 0 ||
        type != MSG_AUTH) {
        proto_send_error(conn, "BAD_REQUEST", "expected AUTH");
        return -1;
    }

    if (kv_get(payload, "id", client_id, MAX_ID_LEN) != 0) {
        proto_send_error(conn, "AUTH_FAIL", "missing credentials");
        return -1;
    }

    int ok = strcmp(client_id, cert_id) == 0 &&
             auth_client_registered(ctx->cfg.client_registry, client_id);

    if (!ok) {
        atomic_fetch_add(&ctx->stats.auth_failures, 1);
        log_msg(LOG_WARN, thread_tag, client_info,
                "AUTH fail id=%s cert_id=%s registered=%d",
                client_id, cert_id,
                auth_client_registered(ctx->cfg.client_registry, cert_id));
        proto_send_error(conn, "AUTH_FAIL", "invalid credentials");
        return -1;
    }

    log_msg(LOG_INFO, thread_tag, client_info,
            "AUTH ok id=%s method=mTLS", client_id);
    return 0;
}

static int handle_client_session(server_context_t *ctx, conn_t *conn,
                                 const char *thread_tag, const char *client_info)
{
    if (proto_send_text(conn, MSG_HELLO, "srv=updctl;proto=1") != 0) {
        return -1;
    }

    char client_id[MAX_ID_LEN] = "";
    if (authenticate_client(ctx, conn, thread_tag, client_info, client_id) != 0) {
        return -1;
    }

    int active_id_reserved = 0;
    int session_rc = -1;
    int reserve_rc = reserve_active_client_id(client_id);
    if (reserve_rc != 0) {
        if (reserve_rc > 0) {
            log_msg(LOG_WARN, thread_tag, client_info,
                    "AUTH duplicate active id=%s", client_id);
            proto_send_error(conn, "DUPLICATE_CLIENT",
                             "client id already has an active session");
        } else {
            log_msg(LOG_WARN, thread_tag, client_info,
                    "AUTH active client registry full id=%s", client_id);
            proto_send_error(conn, "SERVER_BUSY",
                             "server active client registry is full");
        }
        return -1;
    }
    active_id_reserved = 1;

    if (proto_send(conn, MSG_AUTH_OK, 0, NULL, 0) != 0) {
        goto done;
    }

    char active_label[ACTIVE_CLIENT_LABEL];
    snprintf(active_label, sizeof(active_label), "%s (%s)", client_id, client_info);
    stats_active_client_update(&ctx->stats, client_info, active_label);

    char payload[MAX_CONTROL_PAYLOAD + 1];
    msg_type_t type;
    if (recv_control(conn, &type, payload, sizeof(payload), thread_tag, client_info) != 0 ||
        type != MSG_VERSION_REQ) {
        proto_send_error(conn, "BAD_REQUEST", "expected VERSION_REQ");
        goto done;
    }

    char current[MAX_VERSION_STR];
    if (kv_get(payload, "cur", current, sizeof(current)) != 0 || !ver_is_valid(current)) {
        atomic_fetch_add(&ctx->stats.errors, 1);
        proto_send_error(conn, "BAD_VERSION", "invalid semantic version");
        goto done;
    }

    version_manifest_t manifest;
    if (vm_get_snapshot(&ctx->vm, &manifest) != 0) {
        proto_send_error(conn, "IO_ERROR", "manifest unavailable");
        goto done;
    }

    int cmp = 0;
    if (ver_compare_text(current, manifest.latest_version, &cmp) != 0) {
        proto_send_error(conn, "BAD_VERSION", "invalid semantic version");
        goto done;
    }

    if (cmp >= 0) {
        char reply[MAX_CONTROL_PAYLOAD + 1];
        snprintf(reply, sizeof(reply), "latest=%s", manifest.latest_version);
        log_msg(LOG_INFO, thread_tag, client_info,
                "VERSION_CHECK id=%s cur=%s latest=%s decision=UPTODATE",
                client_id, current, manifest.latest_version);
        atomic_fetch_add(&ctx->stats.uptodate_responses, 1);
        if (proto_send_text(conn, MSG_UPTODATE, reply) == 0 &&
            proto_send(conn, MSG_BYE, 0, NULL, 0) == 0) {
            session_rc = 0;
        }
        goto done;
    }

    char package_path[MAX_PATH_LEN * 2];
    if (pkg_resolve_path(ctx->cfg.package_dir, manifest.package_file,
                         package_path, sizeof(package_path)) != 0) {
        atomic_fetch_add(&ctx->stats.errors, 1);
        proto_send_error(conn, "NO_PACKAGE", "package path invalid");
        goto done;
    }

    unsigned long long chunks =
        (manifest.package_size + (unsigned long long)ctx->cfg.chunk_size - 1ULL) /
        (unsigned long long)ctx->cfg.chunk_size;
    char meta[MAX_CONTROL_PAYLOAD + 1];
    snprintf(meta, sizeof(meta),
             "ver=%s;size=%llu;chunk=%d;chunks=%llu;sha256=%s",
             manifest.latest_version, manifest.package_size, ctx->cfg.chunk_size,
             chunks, manifest.checksum_sha256);
    log_msg(LOG_INFO, thread_tag, client_info,
            "VERSION_CHECK id=%s cur=%s latest=%s decision=UPDATE size=%llu",
            client_id, current, manifest.latest_version, manifest.package_size);
    if (proto_send_text(conn, MSG_UPDATE_AVAILABLE, meta) != 0) {
        goto done;
    }

    if (recv_control(conn, &type, payload, sizeof(payload), thread_tag, client_info) != 0 ||
        type != MSG_DOWNLOAD_REQ) {
        proto_send_error(conn, "BAD_REQUEST", "expected DOWNLOAD_REQ");
        goto done;
    }
    unsigned long long offset = kv_get_u64(payload, "offset", manifest.package_size + 1ULL);
    if (offset > manifest.package_size) {
        proto_send_error(conn, "BAD_REQUEST", "offset beyond package size");
        goto done;
    }

    atomic_fetch_add(&ctx->stats.active_transfers, 1);
    int stream_rc = pkg_stream_file(conn, package_path, offset, ctx->cfg.chunk_size,
                                    ctx->cfg.packet_delay_us,
                                    ctx->cfg.pause_hold_timeout_sec,
                                    &ctx->stats,
                                    thread_tag, client_info);
    atomic_fetch_sub(&ctx->stats.active_transfers, 1);
    if (stream_rc != 0) {
        atomic_fetch_add(&ctx->stats.errors, 1);
        goto done;
    }

    char done[MAX_CONTROL_PAYLOAD + 1];
    snprintf(done, sizeof(done), "sha256=%s", manifest.checksum_sha256);
    if (proto_send_text(conn, MSG_TRANSFER_DONE, done) != 0) {
        goto done;
    }

    if (recv_control(conn, &type, payload, sizeof(payload), thread_tag, client_info) != 0 ||
        type != MSG_RESULT) {
        goto done;
    }
    char status[32] = "";
    if (kv_get(payload, "status", status, sizeof(status)) != 0 ||
        strcmp(status, "ok") != 0) {
        atomic_fetch_add(&ctx->stats.errors, 1);
        log_msg(LOG_WARN, thread_tag, client_info, "TRANSFER_RESULT id=%s status=%s",
                client_id, status[0] ? status : "missing");
    } else {
        atomic_fetch_add(&ctx->stats.updates_served, 1);
        log_msg(LOG_INFO, thread_tag, client_info,
                "TRANSFER_DONE id=%s bytes=%llu status=OK",
                client_id, manifest.package_size - offset);
    }

    if (proto_send(conn, MSG_BYE, 0, NULL, 0) == 0) {
        session_rc = 0;
    }

done:
    if (active_id_reserved) {
        release_active_client_id(client_id);
    }
    return session_rc;
}

void *worker_thread(void *arg)
{
    worker_arg_t *warg = (worker_arg_t *)arg;
    server_context_t *ctx = warg->ctx;
    char thread_tag[32];
    snprintf(thread_tag, sizeof(thread_tag), "worker-%02d", warg->worker_id);
    log_msg(LOG_INFO, thread_tag, "-", "WORKER_START");

    tpq_item_t item;
    while (*(ctx->running)) {
        if (tpq_pop(&ctx->queue, &item) != 0) {
            break;
        }
        if (!*(ctx->running)) {
            conn_t closing;
            closing.fd = item.fd;
            closing.ssl = NULL;
            conn_close(&closing);
            break;
        }
        atomic_fetch_add(&ctx->stats.busy_workers, 1);
        stats_active_client_add(&ctx->stats, item.client_info, item.client_info);
        server_track_worker_fd(warg->worker_id, item.fd);
        conn_t conn;
        conn.fd = item.fd;
        conn.ssl = NULL;

        if (ctx->cfg.enable_tls) {
            conn.ssl = tls_accept_connection(ctx->ssl_ctx, conn.fd);
            if (!conn.ssl) {
                atomic_fetch_add(&ctx->stats.errors, 1);
                log_msg(LOG_WARN, thread_tag, item.client_info, "TLS_FAIL handshake");
                conn_close(&conn);
                server_track_worker_fd(warg->worker_id, -1);
                stats_active_client_remove(&ctx->stats, item.client_info);
                atomic_fetch_sub(&ctx->stats.busy_workers, 1);
                continue;
            }
        }

        int rc = handle_client_session(ctx, &conn, thread_tag, item.client_info);
        if (rc != 0) {
            atomic_fetch_add(&ctx->stats.errors, 1);
            log_msg(LOG_WARN, thread_tag, item.client_info, "SESSION_END status=ERROR");
        } else {
            log_msg(LOG_INFO, thread_tag, item.client_info, "SESSION_END status=OK");
        }
        conn_close(&conn);
        server_track_worker_fd(warg->worker_id, -1);
        stats_active_client_remove(&ctx->stats, item.client_info);
        atomic_fetch_sub(&ctx->stats.busy_workers, 1);
    }

    log_msg(LOG_INFO, thread_tag, "-", "WORKER_STOP");
    return NULL;
}
