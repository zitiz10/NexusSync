/* sha1.c — SHA-1 per FIPS 180-4 §6.1. Same block structure and padding as
 * SHA-256, with 80 rounds and 5 state words. WebSocket handshake only. */
#include "crypto/sha1.h"

#include <string.h>

static inline uint32_t rotl(uint32_t x, unsigned n) { return (x << n) | (x >> (32u - n)); }

static void compress(uint32_t h[5], const uint8_t block[64])
{
    uint32_t w[80];
    for (int t = 0; t < 16; t++)
        w[t] = ((uint32_t)block[4 * t] << 24) | ((uint32_t)block[4 * t + 1] << 16) |
               ((uint32_t)block[4 * t + 2] << 8) | (uint32_t)block[4 * t + 3];
    for (int t = 16; t < 80; t++)
        w[t] = rotl(w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int t = 0; t < 80; t++) {
        uint32_t f, k;
        if (t < 20) {
            f = (b & c) | (~b & d);
            k = 0x5a827999;
        } else if (t < 40) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1;
        } else if (t < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdc;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6;
        }
        uint32_t tmp = rotl(a, 5) + f + e + k + w[t];
        e = d;
        d = c;
        c = rotl(b, 30);
        b = a;
        a = tmp;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
}

void nx_sha1_init(nx_sha1_ctx *c)
{
    c->h[0] = 0x67452301;
    c->h[1] = 0xefcdab89;
    c->h[2] = 0x98badcfe;
    c->h[3] = 0x10325476;
    c->h[4] = 0xc3d2e1f0;
    c->total_len = 0;
    c->buf_len = 0;
}

void nx_sha1_update(nx_sha1_ctx *c, const void *data, size_t len)
{
    const uint8_t *p = data;
    c->total_len += len;
    while (len > 0) {
        size_t take = 64 - c->buf_len;
        if (take > len)
            take = len;
        memcpy(c->buf + c->buf_len, p, take);
        c->buf_len += take;
        p += take;
        len -= take;
        if (c->buf_len == 64) {
            compress(c->h, c->buf);
            c->buf_len = 0;
        }
    }
}

void nx_sha1_final(nx_sha1_ctx *c, uint8_t out[NX_SHA1_LEN])
{
    uint64_t bits = c->total_len * 8u;
    c->buf[c->buf_len++] = 0x80;
    if (c->buf_len > 56) {
        memset(c->buf + c->buf_len, 0, 64 - c->buf_len);
        compress(c->h, c->buf);
        c->buf_len = 0;
    }
    memset(c->buf + c->buf_len, 0, 56 - c->buf_len);
    for (int i = 0; i < 8; i++)
        c->buf[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
    compress(c->h, c->buf);
    for (int i = 0; i < 5; i++) {
        out[4 * i] = (uint8_t)(c->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)c->h[i];
    }
    memset(c, 0, sizeof *c);
}

void nx_sha1(const void *data, size_t len, uint8_t out[NX_SHA1_LEN])
{
    nx_sha1_ctx c;
    nx_sha1_init(&c);
    nx_sha1_update(&c, data, len);
    nx_sha1_final(&c, out);
}
