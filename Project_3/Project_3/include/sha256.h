#ifndef SHA256_H
#define SHA256_H

#include "common.h"
#include <stddef.h>

int sha256_buffer(const unsigned char *data, size_t len,
                  unsigned char out_digest[32]);
int sha256_file_hex(const char *path, char out_hex[SHA256_HEX_BUF]);
int sha256_buffer_hex(const unsigned char *data, size_t len,
                      char out_hex[SHA256_HEX_BUF]);
int hex_to_bytes(const char *hex, unsigned char *out, size_t out_len);
void bytes_to_hex(const unsigned char *bytes, size_t len, char *out_hex);

#endif
