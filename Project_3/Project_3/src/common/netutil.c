#include "netutil.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

int net_set_timeouts(int fd, int timeout_sec)
{
    if (timeout_sec <= 0) {
        return 0;
    }
    struct timeval tv;
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
        return -1;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) {
        return -1;
    }
    return 0;
}

int net_create_listener(int port, int backlog)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, backlog) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int wait_for_connect(int fd, int timeout_sec)
{
    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(fd, &wfds);
    struct timeval tv;
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;
    int rc = select(fd + 1, NULL, &wfds, NULL, timeout_sec > 0 ? &tv : NULL);
    if (rc <= 0) {
        return -1;
    }
    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
        errno = err;
        return -1;
    }
    return 0;
}

int net_connect_tcp(const char *host, int port, int timeout_sec)
{
    char service[16];
    snprintf(service, sizeof(service), "%d", port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;

    struct addrinfo *res = NULL;
    if (getaddrinfo(host, service, &hints, &res) != 0) {
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) {
            continue;
        }

        int flags = fcntl(fd, F_GETFL, 0);
        if (timeout_sec > 0 && flags >= 0) {
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }

        int rc = connect(fd, rp->ai_addr, rp->ai_addrlen);
        if (rc == 0 || (rc < 0 && errno == EINPROGRESS &&
                        wait_for_connect(fd, timeout_sec) == 0)) {
            if (timeout_sec > 0 && flags >= 0) {
                fcntl(fd, F_SETFL, flags);
            }
            net_set_timeouts(fd, timeout_sec);
            break;
        }

        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);
    return fd;
}

int net_peer_info(int fd, char *dst, size_t dst_len)
{
    if (!dst || dst_len == 0) {
        return -1;
    }
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    if (getpeername(fd, (struct sockaddr *)&ss, &len) != 0) {
        snprintf(dst, dst_len, "unknown fd=%d", fd);
        return -1;
    }

    char host[INET6_ADDRSTRLEN];
    int port = 0;
    if (ss.ss_family == AF_INET) {
        struct sockaddr_in *in = (struct sockaddr_in *)&ss;
        inet_ntop(AF_INET, &in->sin_addr, host, sizeof(host));
        port = ntohs(in->sin_port);
    } else if (ss.ss_family == AF_INET6) {
        struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)&ss;
        inet_ntop(AF_INET6, &in6->sin6_addr, host, sizeof(host));
        port = ntohs(in6->sin6_port);
    } else {
        snprintf(host, sizeof(host), "unknown");
    }
    snprintf(dst, dst_len, "%s:%d fd=%d", host, port, fd);
    return 0;
}

void conn_close(conn_t *c)
{
    if (!c || c->fd < 0) {
        return;
    }
#if USE_TLS
    if (c->ssl) {
        SSL_shutdown(c->ssl);
        SSL_free(c->ssl);
        c->ssl = NULL;
    }
#else
    c->ssl = NULL;
#endif
    close(c->fd);
    c->fd = -1;
}

int conn_write_all(conn_t *c, const void *buf, size_t len)
{
    const unsigned char *p = (const unsigned char *)buf;
    while (len > 0) {
        int chunk = len > 0x3fffffffU ? 0x3fffffff : (int)len;
        int n;
        if (c->ssl) {
#if USE_TLS
            n = SSL_write(c->ssl, p, chunk);
            if (n <= 0) {
                int err = SSL_get_error(c->ssl, n);
                if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                    continue;
                }
                return -1;
            }
#else
            return -1;
#endif
        } else {
            ssize_t rc = send(c->fd, p, (size_t)chunk, 0);
            if (rc < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return -1;
            }
            if (rc == 0) {
                return -1;
            }
            n = (int)rc;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

int conn_read_all(conn_t *c, void *buf, size_t len)
{
    unsigned char *p = (unsigned char *)buf;
    while (len > 0) {
        int chunk = len > 0x3fffffffU ? 0x3fffffff : (int)len;
        int n;
        if (c->ssl) {
#if USE_TLS
            n = SSL_read(c->ssl, p, chunk);
            if (n <= 0) {
                int err = SSL_get_error(c->ssl, n);
                if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                    continue;
                }
                return -1;
            }
#else
            return -1;
#endif
        } else {
            ssize_t rc = recv(c->fd, p, (size_t)chunk, 0);
            if (rc < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return -1;
            }
            if (rc == 0) {
                return -1;
            }
            n = (int)rc;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

int conn_send_plain_busy(int fd)
{
    static const char busy[] = "BUSY\n";
    size_t off = 0;
    while (off < sizeof(busy) - 1) {
        ssize_t n = send(fd, busy + off, sizeof(busy) - 1 - off, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}
