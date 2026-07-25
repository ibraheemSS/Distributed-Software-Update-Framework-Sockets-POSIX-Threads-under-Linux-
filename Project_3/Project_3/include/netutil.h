#ifndef NETUTIL_H
#define NETUTIL_H

#include <stddef.h>
#ifndef USE_TLS
#define USE_TLS 1
#endif
#if !USE_TLS
#error "TLS encryption is mandatory for this project. Build with USE_TLS=1."
#endif
#if USE_TLS
#include <openssl/ssl.h>
#else
typedef struct ssl_st SSL;
#endif

typedef struct {
    int fd;
    SSL *ssl;
} conn_t;

int net_create_listener(int port, int backlog);
int net_connect_tcp(const char *host, int port, int timeout_sec);
int net_set_timeouts(int fd, int timeout_sec);
int net_peer_info(int fd, char *dst, size_t dst_len);
void conn_close(conn_t *c);
int conn_write_all(conn_t *c, const void *buf, size_t len);
int conn_read_all(conn_t *c, void *buf, size_t len);
int conn_send_plain_busy(int fd);

#endif
