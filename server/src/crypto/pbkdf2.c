/*
 * pbkdf2.c — PBKDF2-HMAC-SHA256 (RFC 8018 §5.2).
 *
 *   T_i = U_1 ^ U_2 ^ ... ^ U_c
 *   U_1 = HMAC(P, S || INT_32_BE(i)),  U_j = HMAC(P, U_{j-1})
 *
 * With 600,000 iterations (SPEC §13) speed matters, so we hash the padded
 * key blocks (K'^ipad, K'^opad) once and reuse the two chaining states.
 * Each iteration then costs exactly two SHA-256 compressions. The output
 * is identical to the naive definition (checked against RFC 7914 vectors
 * and Python hashlib in the tests).
 */
#include "crypto/kdf.h"

#include <string.h>

static inline void store_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* Finishes HMAC for a 32-byte message given the precomputed state after one
 * 64-byte pad block: total length is 64 + 32 = 96 bytes. */
static void finish_32(const uint32_t state[8], const uint8_t msg[32], uint8_t out[32])
{
    uint8_t block[64];
    memcpy(block, msg, 32);
    block[32] = 0x80;
    memset(block + 33, 0, 64 - 33 - 8);
    uint64_t bits = 96u * 8u;
    for (int i = 0; i < 8; i++)
        block[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
    uint32_t h[8];
    memcpy(h, state, sizeof h);
    nx_sha256_compress(h, block);
    for (int i = 0; i < 8; i++)
        store_be32(out + 4 * i, h[i]);
}

void nx_pbkdf2_sha256(const void *password, size_t password_len, const void *salt,
                      size_t salt_len, uint32_t iterations, uint8_t *out, size_t out_len)
{
    uint8_t k[64];
    memset(k, 0, sizeof k);
    if (password_len > 64)
        nx_sha256(password, password_len, k);
    else if (password_len > 0)
        memcpy(k, password, password_len);

    uint8_t pad[64];
    uint32_t istate[8], ostate[8];
    nx_sha256_ctx c;
    nx_sha256_init(&c);
    for (int i = 0; i < 64; i++)
        pad[i] = k[i] ^ 0x36;
    nx_sha256_compress(c.h, pad);
    memcpy(istate, c.h, sizeof istate);
    nx_sha256_init(&c);
    for (int i = 0; i < 64; i++)
        pad[i] = k[i] ^ 0x5c;
    nx_sha256_compress(c.h, pad);
    memcpy(ostate, c.h, sizeof ostate);

    uint32_t block_index = 1;
    size_t produced = 0;
    while (produced < out_len) {
        /* U_1 = HMAC(P, S || INT(i)) through the generic path. */
        uint8_t ibe[4];
        store_be32(ibe, block_index);
        uint8_t u[32], t[32];
        nx_sha256_ctx ic;
        memcpy(ic.h, istate, sizeof istate);
        ic.total_len = 64;
        ic.buf_len = 0;
        nx_sha256_update(&ic, salt, salt_len);
        nx_sha256_update(&ic, ibe, 4);
        uint8_t inner[32];
        nx_sha256_final(&ic, inner);
        finish_32(ostate, inner, u);
        memcpy(t, u, 32);

        for (uint32_t j = 1; j < iterations; j++) {
            finish_32(istate, u, inner); /* inner = H(K^ipad || U) */
            finish_32(ostate, inner, u); /* U = H(K^opad || inner) */
            for (int b = 0; b < 32; b++)
                t[b] ^= u[b];
        }
        size_t take = out_len - produced < 32 ? out_len - produced : 32;
        memcpy(out + produced, t, take);
        produced += take;
        block_index++;
        memset(u, 0, sizeof u);
        memset(t, 0, sizeof t);
        memset(inner, 0, sizeof inner);
    }
    memset(k, 0, sizeof k);
    memset(pad, 0, sizeof pad);
    memset(istate, 0, sizeof istate);
    memset(ostate, 0, sizeof ostate);
}
