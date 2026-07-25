#include "tls.h"
#include <stddef.h>
#include <string.h>

#if USE_TLS
#include <openssl/err.h>
#include <openssl/x509.h>

SSL_CTX *tls_create_server_ctx(const char *cert_path, const char *key_path,
                               const char *ca_path)
{
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
        return NULL;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    if (SSL_CTX_use_certificate_file(ctx, cert_path, SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_use_PrivateKey_file(ctx, key_path, SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(ctx) != 1 ||
        SSL_CTX_load_verify_locations(ctx, ca_path, NULL) != 1) {
        SSL_CTX_free(ctx);
        return NULL;
    }
    SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION);
    SSL_CTX_set_mode(ctx, SSL_MODE_AUTO_RETRY);
    SSL_CTX_set_verify(ctx,
                       SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                       NULL);
    SSL_CTX_set_verify_depth(ctx, 4);
    return ctx;
}

SSL_CTX *tls_create_client_ctx(const char *ca_path, const char *cert_path,
                               const char *key_path)
{
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        return NULL;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    if (SSL_CTX_load_verify_locations(ctx, ca_path, NULL) != 1) {
        SSL_CTX_free(ctx);
        return NULL;
    }
    if (SSL_CTX_use_certificate_file(ctx, cert_path, SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_use_PrivateKey_file(ctx, key_path, SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(ctx) != 1) {
        SSL_CTX_free(ctx);
        return NULL;
    }
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION);
    SSL_CTX_set_mode(ctx, SSL_MODE_AUTO_RETRY);
    return ctx;
}

SSL *tls_accept_connection(SSL_CTX *ctx, int fd)
{
    SSL *ssl = SSL_new(ctx);
    if (!ssl) {
        return NULL;
    }
    SSL_set_fd(ssl, fd);
    if (SSL_accept(ssl) != 1) {
        SSL_free(ssl);
        return NULL;
    }
    return ssl;
}

SSL *tls_connect_connection(SSL_CTX *ctx, int fd,
                            const char *expected_server_name)
{
    SSL *ssl = SSL_new(ctx);
    if (!ssl) {
        return NULL;
    }
    SSL_set_fd(ssl, fd);
    if (expected_server_name && expected_server_name[0] != '\0') {
        SSL_set_tlsext_host_name(ssl, expected_server_name);
        if (SSL_set1_host(ssl, expected_server_name) != 1) {
            SSL_free(ssl);
            return NULL;
        }
    }
    if (SSL_connect(ssl) != 1) {
        SSL_free(ssl);
        return NULL;
    }
    return ssl;
}

void tls_free_ctx(SSL_CTX *ctx)
{
    if (ctx) {
        SSL_CTX_free(ctx);
    }
}

int tls_peer_common_name(SSL *ssl, char *out, size_t out_len)
{
    if (!ssl || !out || out_len == 0) {
        return -1;
    }
    out[0] = '\0';

    X509 *cert = SSL_get_peer_certificate(ssl);
    if (!cert) {
        return -1;
    }

    X509_NAME *subject = X509_get_subject_name(cert);
    int rc = -1;
    if (subject &&
        X509_NAME_get_text_by_NID(subject, NID_commonName,
                                  out, (int)out_len) > 0 &&
        out[0] != '\0') {
        out[out_len - 1] = '\0';
        rc = 0;
    }

    X509_free(cert);
    return rc;
}
#else
SSL_CTX *tls_create_server_ctx(const char *cert_path, const char *key_path,
                               const char *ca_path)
{
    (void)cert_path;
    (void)key_path;
    (void)ca_path;
    return NULL;
}

SSL_CTX *tls_create_client_ctx(const char *ca_path, const char *cert_path,
                               const char *key_path)
{
    (void)ca_path;
    (void)cert_path;
    (void)key_path;
    return NULL;
}

SSL *tls_accept_connection(SSL_CTX *ctx, int fd)
{
    (void)ctx;
    (void)fd;
    return NULL;
}

SSL *tls_connect_connection(SSL_CTX *ctx, int fd,
                            const char *expected_server_name)
{
    (void)ctx;
    (void)fd;
    (void)expected_server_name;
    return NULL;
}

void tls_free_ctx(SSL_CTX *ctx)
{
    (void)ctx;
}

int tls_peer_common_name(SSL *ssl, char *out, size_t out_len)
{
    (void)ssl;
    if (out && out_len > 0) {
        out[0] = '\0';
    }
    return -1;
}
#endif
