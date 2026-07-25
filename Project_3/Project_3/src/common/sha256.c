#include "sha256.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint32_t state[8];
    uint64_t bit_len;
    unsigned char data[64];
    size_t data_len;
} sha256_ctx_t;

static const uint32_t k[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};

static uint32_t rotr(uint32_t x, uint32_t n)
{
    return (x >> n) | (x << (32U - n));
}

static void sha256_transform(sha256_ctx_t *ctx, const unsigned char block[64])
{
    uint32_t m[64];
    for (int i = 0; i < 16; i++) {
        m[i] = ((uint32_t)block[i * 4] << 24) |
               ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) |
               (uint32_t)block[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(m[i - 15], 7) ^ rotr(m[i - 15], 18) ^ (m[i - 15] >> 3);
        uint32_t s1 = rotr(m[i - 2], 17) ^ rotr(m[i - 2], 19) ^ (m[i - 2] >> 10);
        m[i] = m[i - 16] + s0 + m[i - 7] + s1;
    }

    uint32_t a = ctx->state[0];
    uint32_t b = ctx->state[1];
    uint32_t c = ctx->state[2];
    uint32_t d = ctx->state[3];
    uint32_t e = ctx->state[4];
    uint32_t f = ctx->state[5];
    uint32_t g = ctx->state[6];
    uint32_t h = ctx->state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + s1 + ch + k[i] + m[i];
        uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void sha256_init(sha256_ctx_t *ctx)
{
    ctx->data_len = 0;
    ctx->bit_len = 0;
    ctx->state[0] = 0x6a09e667U;
    ctx->state[1] = 0xbb67ae85U;
    ctx->state[2] = 0x3c6ef372U;
    ctx->state[3] = 0xa54ff53aU;
    ctx->state[4] = 0x510e527fU;
    ctx->state[5] = 0x9b05688cU;
    ctx->state[6] = 0x1f83d9abU;
    ctx->state[7] = 0x5be0cd19U;
}

static void sha256_update(sha256_ctx_t *ctx, const unsigned char *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        ctx->data[ctx->data_len++] = data[i];
        if (ctx->data_len == 64) {
            sha256_transform(ctx, ctx->data);
            ctx->bit_len += 512;
            ctx->data_len = 0;
        }
    }
}

static void sha256_final(sha256_ctx_t *ctx, unsigned char digest[32])
{
    size_t i = ctx->data_len;
    ctx->data[i++] = 0x80;
    if (i > 56) {
        while (i < 64) {
            ctx->data[i++] = 0;
        }
        sha256_transform(ctx, ctx->data);
        memset(ctx->data, 0, 56);
    } else {
        while (i < 56) {
            ctx->data[i++] = 0;
        }
    }

    ctx->bit_len += (uint64_t)ctx->data_len * 8ULL;
    ctx->data[56] = (unsigned char)(ctx->bit_len >> 56);
    ctx->data[57] = (unsigned char)(ctx->bit_len >> 48);
    ctx->data[58] = (unsigned char)(ctx->bit_len >> 40);
    ctx->data[59] = (unsigned char)(ctx->bit_len >> 32);
    ctx->data[60] = (unsigned char)(ctx->bit_len >> 24);
    ctx->data[61] = (unsigned char)(ctx->bit_len >> 16);
    ctx->data[62] = (unsigned char)(ctx->bit_len >> 8);
    ctx->data[63] = (unsigned char)(ctx->bit_len);
    sha256_transform(ctx, ctx->data);

    for (int j = 0; j < 8; j++) {
        digest[j * 4] = (unsigned char)(ctx->state[j] >> 24);
        digest[j * 4 + 1] = (unsigned char)(ctx->state[j] >> 16);
        digest[j * 4 + 2] = (unsigned char)(ctx->state[j] >> 8);
        digest[j * 4 + 3] = (unsigned char)(ctx->state[j]);
    }
}

static int hex_nibble(int c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

void bytes_to_hex(const unsigned char *bytes, size_t len, char *out_hex)
{
    static const char alphabet[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out_hex[i * 2] = alphabet[(bytes[i] >> 4) & 0x0f];
        out_hex[i * 2 + 1] = alphabet[bytes[i] & 0x0f];
    }
    out_hex[len * 2] = '\0';
}

int hex_to_bytes(const char *hex, unsigned char *out, size_t out_len)
{
    if (!hex || !out || strlen(hex) != out_len * 2) {
        return -1;
    }
    for (size_t i = 0; i < out_len; i++) {
        int hi = hex_nibble((unsigned char)hex[i * 2]);
        int lo = hex_nibble((unsigned char)hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[i] = (unsigned char)((hi << 4) | lo);
    }
    return 0;
}

int sha256_buffer(const unsigned char *data, size_t len,
                  unsigned char out_digest[32])
{
    if ((!data && len != 0) || !out_digest) {
        return -1;
    }
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, out_digest);
    return 0;
}

int sha256_buffer_hex(const unsigned char *data, size_t len,
                      char out_hex[SHA256_HEX_BUF])
{
    unsigned char digest[32];
    if (sha256_buffer(data, len, digest) != 0) {
        return -1;
    }
    bytes_to_hex(digest, sizeof(digest), out_hex);
    return 0;
}

int sha256_file_hex(const char *path, char out_hex[SHA256_HEX_BUF])
{
    if (!path || !out_hex) {
        return -1;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return -1;
    }

    sha256_ctx_t ctx;
    sha256_init(&ctx);

    unsigned char buf[65536];
    int ok = 1;
    for (;;) {
        size_t n = fread(buf, 1, sizeof(buf), fp);
        if (n > 0) {
            sha256_update(&ctx, buf, n);
        }
        if (n < sizeof(buf)) {
            if (ferror(fp)) {
                ok = 0;
            }
            break;
        }
    }
    fclose(fp);
    if (!ok) {
        return -1;
    }

    unsigned char digest[32];
    sha256_final(&ctx, digest);
    bytes_to_hex(digest, sizeof(digest), out_hex);
    return 0;
}
