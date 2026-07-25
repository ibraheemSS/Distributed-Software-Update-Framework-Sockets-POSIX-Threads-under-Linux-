#include "client_app.h"
#include "logger.h"
#include "netutil.h"
#include "protocol.h"
#include "sha256.h"
#include "tls.h"
#include "version.h"
#include <errno.h>
#include <fcntl.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef enum {
    ATTEMPT_SUCCESS = 0,
    ATTEMPT_RETRY = 1,
    ATTEMPT_RECONNECT = 2,
    ATTEMPT_RESUME_RECONNECT = 3,
    ATTEMPT_FATAL = -1,
    ATTEMPT_PAUSED = -2
} attempt_result_t;

static int client_stop_requested(const client_context_t *ctx)
{
    return ctx && ctx->stop_requested && *(ctx->stop_requested);
}

static void mkdir_p(const char *path)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static void ensure_parent_dir(const char *path)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    char *slash = strrchr(tmp, '/');
    if (slash) {
        *slash = '\0';
        mkdir_p(tmp);
    }
}

static void trim_text(char *s)
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

static int lock_client_version_table(const char *table_path)
{
    char lock_path[MAX_PATH_LEN * 2];
    snprintf(lock_path, sizeof(lock_path), "%s.lock", table_path);
    ensure_parent_dir(lock_path);

    int fd = open(lock_path, O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        return -1;
    }

    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    if (fcntl(fd, F_SETLKW, &fl) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void unlock_client_version_table(int fd)
{
    if (fd < 0) {
        return;
    }
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_UNLCK;
    fl.l_whence = SEEK_SET;
    fcntl(fd, F_SETLK, &fl);
    close(fd);
}

static int parse_version_row(const char *line, char id[MAX_ID_LEN],
                             char version[MAX_VERSION_STR])
{
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", line ? line : "");
    trim_text(tmp);
    if (tmp[0] == '\0' || tmp[0] == '#') {
        return -1;
    }

    char *sep = strchr(tmp, '=');
    if (sep) {
        *sep = '\0';
        trim_text(tmp);
        trim_text(sep + 1);
        if (strlen(tmp) >= MAX_ID_LEN || strlen(sep + 1) >= MAX_VERSION_STR) {
            return -1;
        }
        memcpy(id, tmp, strlen(tmp) + 1);
        memcpy(version, sep + 1, strlen(sep + 1) + 1);
    } else {
        char *p = tmp;
        while (*p && !isspace((unsigned char)*p)) {
            p++;
        }
        if (*p == '\0') {
            return -1;
        }
        *p++ = '\0';
        while (*p && isspace((unsigned char)*p)) {
            p++;
        }
        trim_text(tmp);
        trim_text(p);
        if (strlen(tmp) >= MAX_ID_LEN || strlen(p) >= MAX_VERSION_STR) {
            return -1;
        }
        memcpy(id, tmp, strlen(tmp) + 1);
        memcpy(version, p, strlen(p) + 1);
    }

    trim_text(id);
    trim_text(version);
    return id[0] != '\0' && ver_is_valid(version) ? 0 : -1;
}

static int version_row_ignorable(const char *line)
{
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", line ? line : "");
    trim_text(tmp);
    return tmp[0] == '\0' || tmp[0] == '#';
}

static int remember_version_id(char seen[512][MAX_ID_LEN], int *count,
                               const char *id)
{
    if (!seen || !count || !id || id[0] == '\0') {
        return -1;
    }
    for (int i = 0; i < *count; i++) {
        if (strcmp(seen[i], id) == 0) {
            return -1;
        }
    }
    if (*count >= 512) {
        return -1;
    }
    snprintf(seen[*count], MAX_ID_LEN, "%s", id);
    (*count)++;
    return 0;
}

static unsigned long long file_size_or_zero(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return 0;
    }
    return (unsigned long long)st.st_size;
}

static int interruptible_sleep_ms(const client_context_t *ctx, int total_ms)
{
    int slept = 0;
    while (slept < total_ms && !client_stop_requested(ctx)) {
        int step = (total_ms - slept) < 100 ? (total_ms - slept) : 100;
        usleep((useconds_t)step * 1000U);
        slept += step;
    }
    return client_stop_requested(ctx);
}

static attempt_result_t retry_or_reconnect(const client_context_t *ctx)
{
    return ctx && ctx->resume_retry_active ? ATTEMPT_RECONNECT : ATTEMPT_RETRY;
}

static int wait_for_gui_resume_on_connection(client_context_t *ctx, conn_t *conn)
{
    int waited_ms = 0;
    int timeout_ms = ctx->cfg.pause_timeout_sec * 1000;
    char line[128];

    snprintf(line, sizeof(line), "paused; resume within %d seconds",
             ctx->cfg.pause_timeout_sec);
    client_gui_add_log(&ctx->gui, line);

    while (waited_ms < timeout_ms && !client_stop_requested(ctx)) {
        if (client_gui_consume_resume_request(&ctx->gui)) {
            if (proto_send_text(conn, MSG_RESUME, "status=resume") == 0) {
                client_gui_set_controls(&ctx->gui, 1, 0);
                client_gui_set_progress_visible(&ctx->gui, 1);
                client_gui_set_status(&ctx->gui, "downloading");
                client_gui_add_log(&ctx->gui, "resume requested; continuing download");
                return 0;
            }
            client_gui_set_controls(&ctx->gui, 0, 0);
            client_gui_set_status(&ctx->gui, "reconnecting");
            client_gui_add_log(&ctx->gui, "server pause hold expired; reconnecting now");
            ctx->resume_retry_active = 1;
            return 1;
        }
        usleep(100000);
        waited_ms += 100;
    }

    client_gui_set_controls(&ctx->gui, 0, 0);
    if (client_stop_requested(ctx)) {
        ctx->paused = 1;
        client_gui_set_status(&ctx->gui, "paused");
        return -1;
    }

    ctx->paused = 1;
    client_gui_set_status(&ctx->gui, "paused");
    client_gui_add_log(&ctx->gui, "pause timeout reached; exiting");
    log_msg(LOG_WARN, "client", "-", "PAUSE_TIMEOUT seconds=%d",
            ctx->cfg.pause_timeout_sec);
    return -1;
}

static int recv_control(conn_t *conn, msg_type_t *type, char *payload, size_t cap)
{
    uint8_t flags = 0;
    uint32_t len = 0;
    int rc = proto_recv(conn, type, &flags, payload, cap, &len, 0);
    (void)flags;
    if (rc != PROTO_RECV_OK) {
        return -1;
    }
    payload[len] = '\0';
    return 0;
}

static void close_attempt_connection(conn_t *conn)
{
    if (!conn) {
        return;
    }

    int signal_closed = client_active_fd_was_signal_closed(conn->fd);
    if (signal_closed) {
#if USE_TLS
        if (conn->ssl) {
            SSL_free(conn->ssl);
            conn->ssl = NULL;
        }
#endif
        conn->fd = -1;
        return;
    }

    client_set_active_fd(-1);
    conn_close(conn);
}

static int write_version_file(const client_config_t *cfg, const char *version)
{
    if (!cfg || !version || cfg->client_version_file[0] == '\0' ||
        cfg->client_id[0] == '\0') {
        return -1;
    }

    int lock_fd = lock_client_version_table(cfg->client_version_file);
    if (lock_fd < 0) {
        return -1;
    }

    ensure_parent_dir(cfg->client_version_file);
    char tmp_path[MAX_PATH_LEN * 2];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", cfg->client_version_file);

    char lines[512][256];
    char seen[512][MAX_ID_LEN];
    int seen_count = 0;
    int count = 0;
    int updated = 0;
    FILE *in = fopen(cfg->client_version_file, "r");
    if (in) {
        while (count < 512 && fgets(lines[count], sizeof(lines[count]), in)) {
            if (!strchr(lines[count], '\n') && !feof(in)) {
                fclose(in);
                unlock_client_version_table(lock_fd);
                return -1;
            }
            if (!version_row_ignorable(lines[count])) {
                char id[MAX_ID_LEN];
                char old_version[MAX_VERSION_STR];
                if (parse_version_row(lines[count], id, old_version) != 0 ||
                    remember_version_id(seen, &seen_count, id) != 0) {
                    fclose(in);
                    unlock_client_version_table(lock_fd);
                    return -1;
                }
                if (strcmp(id, cfg->client_id) == 0) {
                    snprintf(lines[count], sizeof(lines[count]), "%s=%s\n",
                             cfg->client_id, version);
                    updated = 1;
                }
            }
            count++;
        }
        fclose(in);
    }
    if (!updated && count >= 512) {
        unlock_client_version_table(lock_fd);
        return -1;
    }
    if (!updated) {
        snprintf(lines[count++], sizeof(lines[0]), "%s=%s\n", cfg->client_id, version);
    }

    FILE *fp = fopen(tmp_path, "w");
    if (!fp) {
        unlock_client_version_table(lock_fd);
        return -1;
    }
    int rc = 0;
    for (int i = 0; i < count; i++) {
        if (fputs(lines[i], fp) == EOF) {
            rc = -1;
            break;
        }
    }
    if (rc == 0 && (fflush(fp) != 0 || fsync(fileno(fp)) != 0)) {
        rc = -1;
    }
    if (fclose(fp) != 0) {
        rc = -1;
    }
    if (rc == 0 && rename(tmp_path, cfg->client_version_file) != 0) {
        rc = -1;
    }
    if (rc != 0) {
        unlink(tmp_path);
    }
    unlock_client_version_table(lock_fd);
    return rc;
}

static void write_resume_metadata(const char *part_path, const char *version,
                                  unsigned long long offset,
                                  unsigned long long total_size,
                                  const char *sha256)
{
    char meta_path[MAX_PATH_LEN * 2];
    int n = snprintf(meta_path, sizeof(meta_path), "%s.resume", part_path);
    if (n <= 0 || (size_t)n >= sizeof(meta_path)) {
        return;
    }

    FILE *fp = fopen(meta_path, "w");
    if (!fp) {
        return;
    }
    fprintf(fp, "version=%s\n", version);
    fprintf(fp, "offset=%llu\n", offset);
    fprintf(fp, "total_size=%llu\n", total_size);
    fprintf(fp, "sha256=%s\n", sha256);
    fprintf(fp, "part_file=%s\n", part_path);
    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);
}

static attempt_result_t save_interrupted_download(client_context_t *ctx, FILE *fp,
                                                  unsigned char *buf,
                                                  const char *part_path,
                                                  const char *version,
                                                  unsigned long long offset,
                                                  unsigned long long total_size,
                                                  const char *sha256,
                                                  const char *status,
                                                  const char *gui_message,
                                                  const char *log_reason,
                                                  attempt_result_t result)
{
    if (fp) {
        fflush(fp);
        fsync(fileno(fp));
        fclose(fp);
    }
    free(buf);
    write_resume_metadata(part_path, version, offset, total_size, sha256);
    if (result == ATTEMPT_PAUSED) {
        ctx->paused = 1;
    } else if (result == ATTEMPT_RECONNECT) {
        ctx->resume_retry_active = 1;
    }
    client_gui_set_progress_visible(&ctx->gui, 0);
    client_gui_set_controls(&ctx->gui, 0, 0);
    client_gui_set_status(&ctx->gui, status);
    client_gui_add_log(&ctx->gui, gui_message);
    log_msg(LOG_WARN, "client", "-",
            "%s version=%s offset=%llu total=%llu part=%s",
            log_reason, version, offset, total_size, part_path);
    return result;
}

int getCurrentVersion(const client_config_t *cfg,
                      char out_version[MAX_VERSION_STR])
{
    if (!cfg || !out_version) {
        return -1;
    }
    if (cfg->current_version[0] != '\0') {
        snprintf(out_version, MAX_VERSION_STR, "%s", cfg->current_version);
        return ver_is_valid(out_version) ? 0 : -1;
    }

    int lock_fd = lock_client_version_table(cfg->client_version_file);
    if (lock_fd < 0) {
        snprintf(out_version, MAX_VERSION_STR, "%s", "0.0.0");
        return 0;
    }

    FILE *fp = fopen(cfg->client_version_file, "r");
    if (!fp) {
        unlock_client_version_table(lock_fd);
        snprintf(out_version, MAX_VERSION_STR, "%s", "0.0.0");
        return 0;
    }

    char line[256];
    char seen[512][MAX_ID_LEN];
    int seen_count = 0;
    int found = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (!strchr(line, '\n') && !feof(fp)) {
            fclose(fp);
            unlock_client_version_table(lock_fd);
            return -1;
        }
        if (version_row_ignorable(line)) {
            continue;
        }
        char id[MAX_ID_LEN];
        char version[MAX_VERSION_STR];
        if (parse_version_row(line, id, version) != 0 ||
            remember_version_id(seen, &seen_count, id) != 0) {
            fclose(fp);
            unlock_client_version_table(lock_fd);
            return -1;
        }
        if (strcmp(id, cfg->client_id) == 0) {
            snprintf(out_version, MAX_VERSION_STR, "%s", version);
            found = 1;
        }
    }

    fclose(fp);
    unlock_client_version_table(lock_fd);
    if (found) {
        return 0;
    }
    snprintf(out_version, MAX_VERSION_STR, "%s", "0.0.0");
    return 0;
}

static attempt_result_t receive_update(client_context_t *ctx, conn_t *conn,
                                       const char *meta)
{
    char latest[MAX_VERSION_STR];
    char sha[SHA256_HEX_BUF];
    if (kv_get(meta, "ver", latest, sizeof(latest)) != 0 ||
        kv_get(meta, "sha256", sha, sizeof(sha)) != 0 || !ver_is_valid(latest)) {
        log_msg(LOG_ERROR, "client", "-", "UPDATE meta invalid");
        return ATTEMPT_FATAL;
    }
    unsigned long long size = kv_get_u64(meta, "size", 0);
    unsigned long long advertised_chunk = kv_get_u64(meta, "chunk", DEFAULT_CHUNK_SIZE);
    int chunk = (int)advertised_chunk;
    if (chunk < MIN_CHUNK_SIZE) {
        chunk = MIN_CHUNK_SIZE;
    }
    if (chunk > MAX_CHUNK_SIZE) {
        chunk = MAX_CHUNK_SIZE;
    }

    client_gui_set_versions(&ctx->gui, NULL, latest);
    mkdir_p(ctx->cfg.download_dir);

    char final_path[MAX_PATH_LEN * 2];
    char part_path[MAX_PATH_LEN * 2];
    int path_n = snprintf(final_path, sizeof(final_path), "%s/app-%s.bin",
                          ctx->cfg.download_dir, latest);
    if (path_n <= 0 || (size_t)path_n >= sizeof(final_path)) {
        log_msg(LOG_ERROR, "client", "-", "DOWNLOAD final path too long");
        return ATTEMPT_FATAL;
    }
    path_n = snprintf(part_path, sizeof(part_path), "%s.part", final_path);
    if (path_n <= 0 || (size_t)path_n >= sizeof(part_path)) {
        log_msg(LOG_ERROR, "client", "-", "DOWNLOAD part path too long");
        return ATTEMPT_FATAL;
    }

    unsigned long long offset = file_size_or_zero(part_path);
    if (offset > size) {
        unlink(part_path);
        offset = 0;
    }

    char req[MAX_CONTROL_PAYLOAD + 1];
    snprintf(req, sizeof(req), "offset=%llu", offset);
    if (proto_send_text(conn, MSG_DOWNLOAD_REQ, req) != 0) {
        if (client_stop_requested(ctx)) {
            return save_interrupted_download(ctx, NULL, NULL, part_path, latest,
                                             offset, size, sha,
                                             "paused",
                                             "download paused; resume data saved",
                                             "DOWNLOAD paused",
                                             ATTEMPT_PAUSED);
        }
        return save_interrupted_download(ctx, NULL, NULL, part_path, latest,
                                         offset, size, sha,
                                         "reconnecting",
                                         "server disconnected; reconnecting in 10 seconds",
                                         "DOWNLOAD server_disconnected",
                                         ATTEMPT_RECONNECT);
    }

    FILE *fp = fopen(part_path, offset > 0 ? "ab" : "wb");
    if (!fp) {
        log_msg(LOG_ERROR, "client", "-", "DOWNLOAD open failed path=%s errno=%d",
                part_path, errno);
        return ATTEMPT_FATAL;
    }

    unsigned char *buf = malloc((size_t)chunk + 1);
    if (!buf) {
        fclose(fp);
        return ATTEMPT_FATAL;
    }

    client_gui_set_status(&ctx->gui, "downloading");
    client_gui_set_progress_visible(&ctx->gui, 1);
    client_gui_set_controls(&ctx->gui, 1, 0);
    client_gui_set_progress(&ctx->gui, offset, size, 0.0);
    log_msg(LOG_INFO, "client", "-", "DOWNLOAD start version=%s size=%llu offset=%llu",
            latest, size, offset);

    unsigned long long received = offset;
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    int done = 0;
    while (!done) {
        if (client_gui_consume_pause_request(&ctx->gui)) {
            char pause_payload[MAX_CONTROL_PAYLOAD + 1];
            snprintf(pause_payload, sizeof(pause_payload), "offset=%llu", received);
            fflush(fp);
            fsync(fileno(fp));
            write_resume_metadata(part_path, latest, received, size, sha);
            client_gui_set_progress_visible(&ctx->gui, 0);
            client_gui_set_controls(&ctx->gui, 0, 1);
            client_gui_set_status(&ctx->gui, "paused");
            client_gui_add_log(&ctx->gui,
                               "download paused from GUI; server hold requested");
            log_msg(LOG_INFO, "client", "-",
                    "DOWNLOAD gui_paused version=%s offset=%llu total=%llu",
                    latest, received, size);

            if (proto_send_text(conn, MSG_PAUSE, pause_payload) != 0) {
                ctx->resume_retry_active = 1;
                return ATTEMPT_RECONNECT;
            }

            int resume_rc = wait_for_gui_resume_on_connection(ctx, conn);
            if (resume_rc == 0) {
                continue;
            }
            if (resume_rc > 0) {
                return ATTEMPT_RESUME_RECONNECT;
            }
            return ATTEMPT_PAUSED;
        }
        if (client_stop_requested(ctx)) {
            return save_interrupted_download(ctx, fp, buf, part_path, latest,
                                             received, size, sha,
                                             "paused",
                                             "download paused; resume data saved",
                                             "DOWNLOAD paused",
                                             ATTEMPT_PAUSED);
        }

        msg_type_t type;
        uint8_t flags;
        uint32_t len;
        int rc = proto_recv(conn, &type, &flags, buf, (size_t)chunk + 1, &len, (size_t)chunk);
        if (rc != PROTO_RECV_OK) {
            if (client_stop_requested(ctx)) {
                return save_interrupted_download(ctx, fp, buf, part_path, latest,
                                                 received, size, sha,
                                                 "paused",
                                                 "download paused; resume data saved",
                                                 "DOWNLOAD paused",
                                                 ATTEMPT_PAUSED);
            }
            return save_interrupted_download(ctx, fp, buf, part_path, latest,
                                             received, size, sha,
                                             "reconnecting",
                                             "server disconnected; reconnecting in 10 seconds",
                                             "DOWNLOAD server_disconnected",
                                             ATTEMPT_RECONNECT);
        }

        if (type == MSG_DATA) {
            if (received + len > size) {
                free(buf);
                fclose(fp);
                client_gui_set_controls(&ctx->gui, 0, 0);
                unlink(part_path);
                log_msg(LOG_ERROR, "client", "-", "DOWNLOAD too many bytes");
                return ATTEMPT_FATAL;
            }
            if (len > 0 && fwrite(buf, 1, len, fp) != len) {
                free(buf);
                fclose(fp);
                client_gui_set_controls(&ctx->gui, 0, 0);
                return ATTEMPT_RETRY;
            }
            received += len;

            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double elapsed = (double)(now.tv_sec - start.tv_sec) +
                             (double)(now.tv_nsec - start.tv_nsec) / 1000000000.0;
            double speed = elapsed > 0.001 ? (double)(received - offset) / elapsed : 0.0;
            client_gui_set_progress(&ctx->gui, received, size, speed);
            if (flags & FLAG_LAST_CHUNK) {
                continue;
            }
        } else if (type == MSG_TRANSFER_DONE) {
            buf[len] = '\0';
            char server_sha[SHA256_HEX_BUF];
            if (kv_get((char *)buf, "sha256", server_sha, sizeof(server_sha)) != 0 ||
                strcasecmp(server_sha, sha) != 0) {
                free(buf);
                fclose(fp);
                client_gui_set_controls(&ctx->gui, 0, 0);
                unlink(part_path);
                return ATTEMPT_RETRY;
            }
            done = 1;
        } else if (type == MSG_ERROR) {
            buf[len] = '\0';
            log_msg(LOG_WARN, "client", "-", "SERVER_ERROR %s", (char *)buf);
            free(buf);
            fclose(fp);
            client_gui_set_controls(&ctx->gui, 0, 0);
            return ATTEMPT_RETRY;
        } else {
            free(buf);
            fclose(fp);
            client_gui_set_controls(&ctx->gui, 0, 0);
            return ATTEMPT_RETRY;
        }
    }

    free(buf);
    if (fflush(fp) != 0 || fsync(fileno(fp)) != 0) {
        fclose(fp);
        client_gui_set_controls(&ctx->gui, 0, 0);
        return ATTEMPT_RETRY;
    }
    fclose(fp);

    char actual[SHA256_HEX_BUF] = "";
    if (sha256_file_hex(part_path, actual) != 0 || strcasecmp(actual, sha) != 0) {
        proto_send_text(conn, MSG_RESULT, "status=checksum_fail");
        unlink(part_path);
        client_gui_set_controls(&ctx->gui, 0, 0);
        client_gui_set_status(&ctx->gui, "checksum failed");
        log_msg(LOG_WARN, "client", "-", "CHECKSUM_FAIL expected=%s actual=%s",
                sha, actual[0] ? actual : "unavailable");
        return ATTEMPT_RETRY;
    }

    if (rename(part_path, final_path) != 0) {
        log_msg(LOG_ERROR, "client", "-", "RENAME failed part=%s final=%s errno=%d",
                part_path, final_path, errno);
        client_gui_set_controls(&ctx->gui, 0, 0);
        return ATTEMPT_FATAL;
    }

    proto_send_text(conn, MSG_RESULT, "status=ok");
    msg_type_t type;
    char payload[MAX_CONTROL_PAYLOAD + 1];
    recv_control(conn, &type, payload, sizeof(payload));
    if (write_version_file(&ctx->cfg, latest) != 0) {
        client_gui_set_controls(&ctx->gui, 0, 0);
        client_gui_set_status(&ctx->gui, "failed");
        log_msg(LOG_ERROR, "client", "-",
                "VERSION_TABLE update failed path=%s id=%s",
                ctx->cfg.client_version_file, ctx->cfg.client_id);
        return ATTEMPT_FATAL;
    }
    char resume_path[MAX_PATH_LEN * 2];
    int resume_n = snprintf(resume_path, sizeof(resume_path), "%s.resume", part_path);
    if (resume_n <= 0 || (size_t)resume_n >= sizeof(resume_path)) {
        client_gui_set_controls(&ctx->gui, 0, 0);
        client_gui_set_status(&ctx->gui, "failed");
        log_msg(LOG_ERROR, "client", "-", "RESUME path too long part=%s", part_path);
        return ATTEMPT_FATAL;
    }
    unlink(resume_path);
    client_gui_set_progress_visible(&ctx->gui, 0);
    client_gui_set_progress(&ctx->gui, 0, 0, 0.0);
    client_gui_set_controls(&ctx->gui, 0, 0);
    client_gui_set_status(&ctx->gui, "installed");
    client_gui_add_log(&ctx->gui, "update installed; SHA-256 verified");
    ctx->resume_retry_active = 0;
    printf("Update installed: %s -> %s\n", final_path, latest);
    log_msg(LOG_INFO, "client", "-", "DOWNLOAD complete file=%s version=%s", final_path, latest);
    return ATTEMPT_SUCCESS;
}

static attempt_result_t run_attempt(client_context_t *ctx,
                                    const char current[MAX_VERSION_STR])
{
    client_gui_set_status(&ctx->gui,
                          ctx->resume_retry_active ? "reconnecting" : "connecting");
    int fd = net_connect_tcp(ctx->cfg.server_host, ctx->cfg.server_port, ctx->cfg.timeout_sec);
    if (fd < 0) {
        log_msg(LOG_WARN, "client", "-", "CONNECT failed host=%s port=%d",
                ctx->cfg.server_host, ctx->cfg.server_port);
        return client_stop_requested(ctx) ? ATTEMPT_PAUSED : retry_or_reconnect(ctx);
    }
    client_set_active_fd(fd);

    conn_t conn;
    conn.fd = fd;
    conn.ssl = NULL;
    if (client_stop_requested(ctx)) {
        close_attempt_connection(&conn);
        return ATTEMPT_PAUSED;
    }
    if (ctx->cfg.enable_tls) {
        conn.ssl = tls_connect_connection(ctx->ssl_ctx, fd,
                                          ctx->cfg.server_tls_name);
        if (!conn.ssl) {
            log_msg(LOG_WARN, "client", "-", "TLS_CONNECT failed");
            close_attempt_connection(&conn);
            return client_stop_requested(ctx) ? ATTEMPT_PAUSED : retry_or_reconnect(ctx);
        }
    }

    char payload[MAX_CONTROL_PAYLOAD + 1];
    msg_type_t type;
    if (recv_control(&conn, &type, payload, sizeof(payload)) != 0 || type != MSG_HELLO) {
        close_attempt_connection(&conn);
        return client_stop_requested(ctx) ? ATTEMPT_PAUSED : retry_or_reconnect(ctx);
    }

    client_gui_set_status(&ctx->gui, "authenticating");
    char auth_payload[MAX_CONTROL_PAYLOAD + 1];
    snprintf(auth_payload, sizeof(auth_payload), "id=%s", ctx->cfg.client_id);
    if (proto_send_text(&conn, MSG_AUTH, auth_payload) != 0 ||
        recv_control(&conn, &type, payload, sizeof(payload)) != 0) {
        close_attempt_connection(&conn);
        return client_stop_requested(ctx) ? ATTEMPT_PAUSED : retry_or_reconnect(ctx);
    }
    if (type == MSG_ERROR) {
        char code[64] = "";
        kv_get(payload, "code", code, sizeof(code));
        log_msg(LOG_WARN, "client", "-", "SERVER_ERROR %s", payload);
        if (strcmp(code, "DUPLICATE_CLIENT") == 0) {
            client_gui_set_progress_visible(&ctx->gui, 0);
            client_gui_set_controls(&ctx->gui, 0, 0);
            client_gui_set_status(&ctx->gui, "failed");
            client_gui_add_log(&ctx->gui,
                               "client ID already in use");
            client_errorf("client: %s is already in use. Close the other client terminal or choose another ID",
                          ctx->cfg.client_id);
            close_attempt_connection(&conn);
            return ATTEMPT_FATAL;
        }
        if (strcmp(code, "AUTH_FAIL") == 0) {
            client_gui_set_progress_visible(&ctx->gui, 0);
            client_gui_set_controls(&ctx->gui, 0, 0);
            client_gui_set_status(&ctx->gui, "failed");
            client_gui_add_log(&ctx->gui,
                               "authentication failed; check client certificate/id");
            close_attempt_connection(&conn);
            return ATTEMPT_FATAL;
        }
        close_attempt_connection(&conn);
        return ATTEMPT_RETRY;
    }
    if (type != MSG_AUTH_OK) {
        close_attempt_connection(&conn);
        return client_stop_requested(ctx) ? ATTEMPT_PAUSED : retry_or_reconnect(ctx);
    }

    char req[MAX_CONTROL_PAYLOAD + 1];
    snprintf(req, sizeof(req), "cur=%s", current);
    if (proto_send_text(&conn, MSG_VERSION_REQ, req) != 0 ||
        recv_control(&conn, &type, payload, sizeof(payload)) != 0) {
        close_attempt_connection(&conn);
        return client_stop_requested(ctx) ? ATTEMPT_PAUSED : retry_or_reconnect(ctx);
    }

    if (type == MSG_UPTODATE) {
        char latest[MAX_VERSION_STR] = "";
        kv_get(payload, "latest", latest, sizeof(latest));
        client_gui_set_versions(&ctx->gui, current, latest[0] ? latest : current);
        client_gui_set_progress_visible(&ctx->gui, 0);
        client_gui_set_progress(&ctx->gui, 0, 0, 0.0);
        client_gui_set_controls(&ctx->gui, 0, 0);
        client_gui_set_status(&ctx->gui, "up to date");
        client_gui_add_log(&ctx->gui, "already up to date; closing in 10 seconds");
        printf("Already up to date (current=%s latest=%s)\n",
               current, latest[0] ? latest : current);
        recv_control(&conn, &type, payload, sizeof(payload));
        close_attempt_connection(&conn);
        ctx->resume_retry_active = 0;
        if (interruptible_sleep_ms(ctx, 10000)) {
            return ATTEMPT_PAUSED;
        }
        return ATTEMPT_SUCCESS;
    }

    if (type == MSG_UPDATE_AVAILABLE) {
        attempt_result_t rc = receive_update(ctx, &conn, payload);
        close_attempt_connection(&conn);
        if (rc == ATTEMPT_SUCCESS &&
            interruptible_sleep_ms(ctx, 10000)) {
            return ATTEMPT_PAUSED;
        }
        return rc;
    }

    if (type == MSG_ERROR) {
        char code[64] = "";
        kv_get(payload, "code", code, sizeof(code));
        log_msg(LOG_WARN, "client", "-", "SERVER_ERROR %s", payload);
        if (strcmp(code, "DUPLICATE_CLIENT") == 0) {
            client_gui_set_progress_visible(&ctx->gui, 0);
            client_gui_set_controls(&ctx->gui, 0, 0);
            client_gui_set_status(&ctx->gui, "failed");
            client_gui_add_log(&ctx->gui,
                               "client ID already in use");
            client_errorf("client: %s is already in use. Close the other client terminal or choose another ID",
                          ctx->cfg.client_id);
            close_attempt_connection(&conn);
            return ATTEMPT_FATAL;
        }
        close_attempt_connection(&conn);
        return strcmp(code, "BAD_VERSION") == 0 ? ATTEMPT_FATAL : ATTEMPT_RETRY;
    }

    close_attempt_connection(&conn);
    return client_stop_requested(ctx) ? ATTEMPT_PAUSED : retry_or_reconnect(ctx);
}

int CheckForUpdate(client_context_t *ctx)
{
    if (!ctx) {
        return -1;
    }
    srand((unsigned int)time(NULL) ^ (unsigned int)getpid());

    char current[MAX_VERSION_STR];
    if (getCurrentVersion(&ctx->cfg, current) != 0) {
        client_gui_set_status(&ctx->gui, "failed");
        log_msg(LOG_ERROR, "client", "-", "CURRENT_VERSION invalid");
        return -1;
    }
    client_gui_set_versions(&ctx->gui, current, NULL);
    client_gui_set_progress_visible(&ctx->gui, 0);
    client_gui_set_progress(&ctx->gui, 0, 0, 0.0);
    client_gui_set_controls(&ctx->gui, 0, 0);
    client_gui_add_log(&ctx->gui, "checking for update");
    log_msg(LOG_INFO, "client", "-", "CHECK_FOR_UPDATE current=%s", current);

    for (int attempt = 1; attempt <= ctx->cfg.retry_max; attempt++) {
        if (client_stop_requested(ctx)) {
            ctx->paused = 1;
            client_gui_set_status(&ctx->gui, "paused");
            return 130;
        }
        attempt_result_t rc = run_attempt(ctx, current);
        if (rc == ATTEMPT_SUCCESS) {
            return 0;
        }
        if (rc == ATTEMPT_RESUME_RECONNECT) {
            if (attempt < ctx->cfg.retry_max) {
                client_gui_set_progress_visible(&ctx->gui, 0);
                client_gui_set_controls(&ctx->gui, 0, 0);
                client_gui_set_status(&ctx->gui, "reconnecting");
                client_gui_add_log(&ctx->gui,
                                   "resume reconnecting immediately");
                log_msg(LOG_WARN, "client", "-",
                        "RESUME_RECONNECT_NOW attempt=%d max_attempts=%d",
                        attempt, ctx->cfg.retry_max);
                attempt--;
                continue;
            }
            break;
        }
        if (rc == ATTEMPT_RECONNECT) {
            if (attempt < ctx->cfg.retry_max) {
                char line[128];
                snprintf(line, sizeof(line),
                         "trying to reconnect in 10 seconds (attempt %d/%d)",
                         attempt + 1, ctx->cfg.retry_max);
                client_gui_set_progress_visible(&ctx->gui, 0);
                client_gui_set_controls(&ctx->gui, 0, 0);
                client_gui_set_status(&ctx->gui, "reconnecting");
                client_gui_add_log(&ctx->gui, line);
                log_msg(LOG_WARN, "client", "-",
                        "RECONNECT_WAIT ms=10000 next_attempt=%d max_attempts=%d",
                        attempt + 1, ctx->cfg.retry_max);
                if (interruptible_sleep_ms(ctx, 10000)) {
                    ctx->paused = 1;
                    client_gui_set_status(&ctx->gui, "paused");
                    return 130;
                }
                continue;
            }
            break;
        }
        if (rc == ATTEMPT_PAUSED) {
            return 130;
        }
        if (rc == ATTEMPT_FATAL) {
            client_gui_set_status(&ctx->gui, "failed");
            return -1;
        }
        if (attempt < ctx->cfg.retry_max) {
            char line[96];
            snprintf(line, sizeof(line), "retrying attempt %d/%d", attempt + 1, ctx->cfg.retry_max);
            client_gui_add_log(&ctx->gui, line);
            client_gui_set_controls(&ctx->gui, 0, 0);
            client_gui_set_status(&ctx->gui, "retrying");
            if (interruptible_sleep_ms(ctx, ctx->cfg.retry_backoff_ms * attempt)) {
                ctx->paused = 1;
                return 130;
            }
        }
    }

    client_gui_set_status(&ctx->gui, "failed");
    log_msg(LOG_ERROR, "client", "-", "CHECK_FOR_UPDATE failed after retries");
    return -1;
}
