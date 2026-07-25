#include "package_service.h"
#include "logger.h"
#include "protocol.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>

static int recv_control_if_ready(conn_t *conn, msg_type_t *type,
                                 char *payload, size_t payload_len,
                                 int timeout_ms)
{
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(conn->fd, &rfds);

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int ready = select(conn->fd + 1, &rfds, NULL, NULL, &tv);
    if (ready <= 0) {
        return ready;
    }

    uint8_t flags = 0;
    uint32_t len = 0;
    int rc = proto_recv(conn, type, &flags, payload, payload_len, &len, 0);
    (void)flags;
    if (rc != PROTO_RECV_OK) {
        return -1;
    }
    payload[len] = '\0';
    return 1;
}

static int handle_pause_if_requested(conn_t *conn, int pause_hold_timeout_sec,
                                     unsigned long long offset,
                                     const char *thread_tag,
                                     const char *client_info)
{
    char payload[MAX_CONTROL_PAYLOAD + 1];
    msg_type_t type;
    int rc = recv_control_if_ready(conn, &type, payload, sizeof(payload), 0);
    if (rc <= 0) {
        return rc;
    }
    if (type != MSG_PAUSE) {
        log_msg(LOG_WARN, thread_tag, client_info,
                "UNEXPECTED_CONTROL during_stream type=%s",
                proto_msg_name(type));
        return -1;
    }

    unsigned long long client_offset = kv_get_u64(payload, "offset", offset);
    log_msg(LOG_INFO, thread_tag, client_info,
            "STREAM_PAUSED client_offset=%llu server_offset=%llu hold_sec=%d",
            client_offset, offset, pause_hold_timeout_sec);

    rc = recv_control_if_ready(conn, &type, payload, sizeof(payload),
                               pause_hold_timeout_sec * 1000);
    if (rc == 0) {
        log_msg(LOG_WARN, thread_tag, client_info,
                "STREAM_PAUSE_TIMEOUT hold_sec=%d offset=%llu",
                pause_hold_timeout_sec, offset);
        return -1;
    }
    if (rc < 0 || type != MSG_RESUME) {
        log_msg(LOG_WARN, thread_tag, client_info,
                "STREAM_RESUME_FAILED type=%s", rc < 0 ? "recv_error" : proto_msg_name(type));
        return -1;
    }

    log_msg(LOG_INFO, thread_tag, client_info,
            "STREAM_RESUMED offset=%llu", offset);
    return 0;
}

int pkg_resolve_path(const char *package_dir, const char *package_file,
                     char *out, size_t out_len)
{
    if (!package_dir || !package_file || !out || out_len == 0 ||
        package_file[0] == '\0' || package_file[0] == '/' ||
        strstr(package_file, "..") || strchr(package_file, '/') ||
        strchr(package_file, '\\')) {
        return -1;
    }
    int n = snprintf(out, out_len, "%s/%s", package_dir, package_file);
    return (n > 0 && (size_t)n < out_len) ? 0 : -1;
}

int pkg_file_size(const char *path, unsigned long long *size_out)
{
    struct stat st;
    if (!path || !size_out || stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return -1;
    }
    *size_out = (unsigned long long)st.st_size;
    return 0;
}

int pkg_stream_file(conn_t *conn, const char *path, unsigned long long offset,
                    int chunk_size, int packet_delay_us, int pause_hold_timeout_sec,
                    stats_t *stats,
                    const char *thread_tag, const char *client_info)
{
    unsigned long long size = 0;
    if (pkg_file_size(path, &size) != 0 || offset > size ||
        chunk_size < MIN_CHUNK_SIZE || chunk_size > MAX_CHUNK_SIZE) {
        return -1;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return -1;
    }
    if (fseeko(fp, (off_t)offset, SEEK_SET) != 0) {
        fclose(fp);
        return -1;
    }

    unsigned char *buf = malloc((size_t)chunk_size);
    if (!buf) {
        fclose(fp);
        return -1;
    }

    unsigned long long sent = offset;
    int rc = 0;
    while (sent < size) {
        if (handle_pause_if_requested(conn, pause_hold_timeout_sec, sent,
                                      thread_tag, client_info) != 0) {
            rc = -1;
            break;
        }
        unsigned long long remaining = size - sent;
        size_t want = remaining < (unsigned long long)chunk_size ?
                      (size_t)remaining : (size_t)chunk_size;
        size_t n = fread(buf, 1, want, fp);
        if (n == 0) {
            rc = -1;
            break;
        }
        sent += (unsigned long long)n;
        uint8_t flags = (sent == size) ? FLAG_LAST_CHUNK : 0;
        if (proto_send(conn, MSG_DATA, flags, buf, (uint32_t)n) != 0) {
            rc = -1;
            break;
        }
        if (stats) {
            atomic_fetch_add(&stats->bytes_sent, (unsigned long long)n);
        }
        if (packet_delay_us > 0 && sent < size) {
            usleep((useconds_t)packet_delay_us);
        }
        if (n < want && ferror(fp)) {
            rc = -1;
            break;
        }
    }

    free(buf);
    fclose(fp);
    if (rc == 0) {
        log_msg(LOG_INFO, thread_tag, client_info,
                "PACKAGE_STREAM complete path=%s bytes=%llu offset=%llu chunk=%d delay_us=%d",
                path, size - offset, offset, chunk_size, packet_delay_us);
    }
    return rc;
}
