#ifndef TLS_H
#define TLS_H

#ifndef USE_TLS
#define USE_TLS 1
#endif
#if !USE_TLS
#error "TLS encryption is mandatory for this project. Build with USE_TLS=1."
#endif
#if USE_TLS
#include <openssl/ssl.h>
#else
typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_st SSL;
#endif

SSL_CTX *tls_create_server_ctx(const char *cert_path, const char *key_path,
                               const char *ca_path);
SSL_CTX *tls_create_client_ctx(const char *ca_path, const char *cert_path,
                               const char *key_path);
SSL *tls_accept_connection(SSL_CTX *ctx, int fd);
SSL *tls_connect_connection(SSL_CTX *ctx, int fd,
                            const char *expected_server_name);
void tls_free_ctx(SSL_CTX *ctx);
int tls_peer_common_name(SSL *ssl, char *out, size_t out_len);

#endif
