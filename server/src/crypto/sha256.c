/*
 * sha256.c — SHA-256 per FIPS 180-4 §6.2.
 *
 * The algorithm processes the message in 64-byte blocks. Each block is
 * expanded into 64 32-bit words (the "message schedule") and mixed into
 * eight 32-bit state words over 64 rounds. The message is padded with a
 * 0x80 byte, zeros, and the bit length as a big-endian 64-bit number so
 * that the total is a multiple of 64 bytes.
 */
#include "crypto/sha256.h"

#include <string.h>

/* First 32 bits of the fractional parts of the cube roots of the first
 * 64 primes (FIPS 180-4 §4.2.2). */
static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static inline uint32_t rotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32u - n)); }

static inline uint32_t load_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline void store_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

void nx_sha256_compress(uint32_t h[8], const uint8_t block[64])
{
    uint32_t w[64];
    for (int t = 0; t < 16; t++)
        w[t] = load_be32(block + 4 * t);
    for (int t = 16; t < 64; t++) {
        uint32_t s0 = rotr(w[t - 15], 7) ^ rotr(w[t - 15], 18) ^ (w[t - 15] >> 3);
        uint32_t s1 = rotr(w[t - 2], 17) ^ rotr(w[t - 2], 19) ^ (w[t - 2] >> 10);
        w[t] = w[t - 16] + s0 + w[t - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int t = 0; t < 64; t++) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + S1 + ch + K[t] + w[t];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
}

void nx_sha256_init(nx_sha256_ctx *c)
{
    /* First 32 bits of the fractional parts of the square roots of the
     * first 8 primes (FIPS 180-4 §5.3.3). */
    static const uint32_t IV[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(c->h, IV, sizeof IV);
    c->total_len = 0;
    c->buf_len = 0;
}

void nx_sha256_update(nx_sha256_ctx *c, const void *data, size_t len)
{
    const uint8_t *p = data;
    c->total_len += len;
    if (c->buf_len > 0) {
        size_t take = 64 - c->buf_len;
        if (take > len)
            take = len;
        memcpy(c->buf + c->buf_len, p, take);
        c->buf_len += take;
        p += take;
        len -= take;
        if (c->buf_len < 64)
            return;
        nx_sha256_compress(c->h, c->buf);
        c->buf_len = 0;
    }
    while (len >= 64) {
        nx_sha256_compress(c->h, p);
        p += 64;
        len -= 64;
    }
    if (len > 0) {
        memcpy(c->buf, p, len);
        c->buf_len = len;
    }
}

void nx_sha256_final(nx_sha256_ctx *c, uint8_t out[NX_SHA256_LEN])
{
    uint64_t bits = c->total_len * 8u;
    /* Padding: 0x80, then zeros until 56 mod 64, then the 64-bit length. */
    c->buf[c->buf_len++] = 0x80;
    if (c->buf_len > 56) {
        memset(c->buf + c->buf_len, 0, 64 - c->buf_len);
        nx_sha256_compress(c->h, c->buf);
        c->buf_len = 0;
    }
    memset(c->buf + c->buf_len, 0, 56 - c->buf_len);
    for (int i = 0; i < 8; i++)
        c->buf[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
    nx_sha256_compress(c->h, c->buf);
    for (int i = 0; i < 8; i++)
        store_be32(out + 4 * i, c->h[i]);
    memset(c, 0, sizeof *c); /* do not leave key-derived state lying around */
}

void nx_sha256(const void *data, size_t len, uint8_t out[NX_SHA256_LEN])
{
    nx_sha256_ctx c;
    nx_sha256_init(&c);
    nx_sha256_update(&c, data, len);
    nx_sha256_final(&c, out);
}

void nx_hex(const uint8_t *bytes, size_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = digits[bytes[i] >> 4];
        out[2 * i + 1] = digits[bytes[i] & 15];
    }
    out[2 * n] = '\0';
}

static int hexval(char ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
    return -1;
}

int nx_unhex(const char *hex, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int hi = hexval(hex[2 * i]);
        int lo = hi < 0 ? -1 : hexval(hex[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}
