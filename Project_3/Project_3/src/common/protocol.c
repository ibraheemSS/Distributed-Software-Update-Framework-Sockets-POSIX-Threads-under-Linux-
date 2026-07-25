#include "protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put_u32_be(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)((v >> 24) & 0xff);
    p[1] = (unsigned char)((v >> 16) & 0xff);
    p[2] = (unsigned char)((v >> 8) & 0xff);
    p[3] = (unsigned char)(v & 0xff);
}

static uint32_t get_u32_be(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

int proto_send(conn_t *conn, msg_type_t type, uint8_t flags,
               const void *payload, uint32_t payload_len)
{
    if (!conn || (!payload && payload_len > 0)) {
        return -1;
    }
    if (type != MSG_DATA && payload_len > MAX_CONTROL_PAYLOAD) {
        return -1;
    }

    unsigned char hdr[FRAME_HEADER_SIZE];
    hdr[0] = FRAME_MAGIC0;
    hdr[1] = FRAME_MAGIC1;
    hdr[2] = FRAME_MAGIC2;
    hdr[3] = FRAME_MAGIC3;
    hdr[4] = PROTO_VERSION;
    hdr[5] = (uint8_t)type;
    hdr[6] = flags;
    hdr[7] = 0;
    put_u32_be(hdr + 8, payload_len);

    if (conn_write_all(conn, hdr, sizeof(hdr)) != 0) {
        return -1;
    }
    if (payload_len > 0 && conn_write_all(conn, payload, payload_len) != 0) {
        return -1;
    }
    return 0;
}

int proto_send_text(conn_t *conn, msg_type_t type, const char *text)
{
    uint32_t len = text ? (uint32_t)strlen(text) : 0;
    return proto_send(conn, type, 0, text, len);
}

int proto_send_error(conn_t *conn, const char *code, const char *message)
{
    char payload[MAX_CONTROL_PAYLOAD + 1];
    snprintf(payload, sizeof(payload), "code=%s;msg=%s",
             code ? code : "IO_ERROR", message ? message : "");
    return proto_send_text(conn, MSG_ERROR, payload);
}

int proto_recv(conn_t *conn, msg_type_t *type, uint8_t *flags, void *payload,
               size_t payload_cap, uint32_t *payload_len,
               size_t data_payload_cap)
{
    unsigned char hdr[FRAME_HEADER_SIZE];
    if (conn_read_all(conn, hdr, sizeof(hdr)) != 0) {
        return PROTO_RECV_EOF;
    }
    if (hdr[0] != FRAME_MAGIC0 || hdr[1] != FRAME_MAGIC1 ||
        hdr[2] != FRAME_MAGIC2 || hdr[3] != FRAME_MAGIC3 ||
        hdr[4] != PROTO_VERSION || hdr[7] != 0) {
        return PROTO_RECV_BAD;
    }

    msg_type_t mt = (msg_type_t)hdr[5];
    uint32_t len = get_u32_be(hdr + 8);
    size_t allowed = (mt == MSG_DATA) ? data_payload_cap : MAX_CONTROL_PAYLOAD;
    if (len > allowed || len > payload_cap) {
        return PROTO_RECV_TOO_LARGE;
    }
    if (len > 0 && conn_read_all(conn, payload, len) != 0) {
        return PROTO_RECV_EOF;
    }
    if (payload && payload_cap > len) {
        ((char *)payload)[len] = '\0';
    }

    if (type) {
        *type = mt;
    }
    if (flags) {
        *flags = hdr[6];
    }
    if (payload_len) {
        *payload_len = len;
    }
    return PROTO_RECV_OK;
}

int kv_get(const char *payload, const char *key, char *out, size_t out_len)
{
    if (!payload || !key || !out || out_len == 0) {
        return -1;
    }
    size_t key_len = strlen(key);
    const char *p = payload;
    while (*p) {
        const char *end = strchr(p, ';');
        size_t part_len = end ? (size_t)(end - p) : strlen(p);
        const char *eq = memchr(p, '=', part_len);
        if (eq && (size_t)(eq - p) == key_len && strncmp(p, key, key_len) == 0) {
            size_t value_len = part_len - key_len - 1;
            if (value_len >= out_len) {
                return -1;
            }
            memcpy(out, eq + 1, value_len);
            out[value_len] = '\0';
            return 0;
        }
        if (!end) {
            break;
        }
        p = end + 1;
    }
    return -1;
}

uint64_t kv_get_u64(const char *payload, const char *key, uint64_t fallback)
{
    char tmp[64];
    if (kv_get(payload, key, tmp, sizeof(tmp)) != 0) {
        return fallback;
    }
    char *end = NULL;
    unsigned long long v = strtoull(tmp, &end, 10);
    if (!end || *end != '\0') {
        return fallback;
    }
    return (uint64_t)v;
}

const char *proto_msg_name(msg_type_t type)
{
    switch (type) {
    case MSG_HELLO: return "HELLO";
    case MSG_AUTH: return "AUTH";
    case MSG_AUTH_OK: return "AUTH_OK";
    case MSG_VERSION_REQ: return "VERSION_REQ";
    case MSG_UPTODATE: return "UPTODATE";
    case MSG_UPDATE_AVAILABLE: return "UPDATE_AVAILABLE";
    case MSG_DOWNLOAD_REQ: return "DOWNLOAD_REQ";
    case MSG_DATA: return "DATA";
    case MSG_TRANSFER_DONE: return "TRANSFER_DONE";
    case MSG_RESULT: return "RESULT";
    case MSG_ERROR: return "ERROR";
    case MSG_BYE: return "BYE";
    case MSG_PAUSE: return "PAUSE";
    case MSG_RESUME: return "RESUME";
    default: return "UNKNOWN";
    }
}
