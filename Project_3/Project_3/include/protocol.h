#ifndef PROTOCOL_H
#define PROTOCOL_H

#include "common.h"
#include "netutil.h"
#include <stddef.h>
#include <stdint.h>

typedef enum {
    MSG_HELLO            = 1,
    MSG_AUTH             = 2,
    MSG_AUTH_OK          = 3,
    MSG_VERSION_REQ      = 4,
    MSG_UPTODATE         = 5,
    MSG_UPDATE_AVAILABLE = 6,
    MSG_DOWNLOAD_REQ     = 7,
    MSG_DATA             = 8,
    MSG_TRANSFER_DONE    = 9,
    MSG_RESULT           = 10,
    MSG_ERROR            = 11,
    MSG_BYE              = 12,
    MSG_PAUSE            = 13,
    MSG_RESUME           = 14
} msg_type_t;

#define FLAG_LAST_CHUNK 0x01

typedef enum {
    PROTO_RECV_OK = 0,
    PROTO_RECV_EOF = 1,
    PROTO_RECV_BAD = -1,
    PROTO_RECV_TOO_LARGE = -2
} proto_recv_status_t;

int proto_send(conn_t *conn, msg_type_t type, uint8_t flags,
               const void *payload, uint32_t payload_len);
int proto_send_text(conn_t *conn, msg_type_t type, const char *text);
int proto_send_error(conn_t *conn, const char *code, const char *message);
int proto_recv(conn_t *conn, msg_type_t *type, uint8_t *flags, void *payload,
               size_t payload_cap, uint32_t *payload_len,
               size_t data_payload_cap);
int kv_get(const char *payload, const char *key, char *out, size_t out_len);
uint64_t kv_get_u64(const char *payload, const char *key, uint64_t fallback);
const char *proto_msg_name(msg_type_t type);

#endif
