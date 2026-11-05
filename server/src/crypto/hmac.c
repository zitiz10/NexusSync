/*
 * hmac.c — HMAC-SHA256 (RFC 2104):
 *   HMAC(K, m) = H((K' ^ opad) || H((K' ^ ipad) || m))
 * where K' is the key padded to the 64-byte block (hashed first if longer).
 */
#include "crypto/kdf.h"

#include <string.h>

void nx_hmac_sha256(const void *key, size_t key_len, const void *msg, size_t msg_len,
                    uint8_t out[NX_SHA256_LEN])
{
    uint8_t k[64];
    memset(k, 0, sizeof k);
    if (key_len > 64)
        nx_sha256(key, key_len, k);
    else if (key_len > 0)
        memcpy(k, key, key_len);

    uint8_t pad[64];
    for (int i = 0; i < 64; i++)
        pad[i] = k[i] ^ 0x36;
    nx_sha256_ctx c;
    uint8_t inner[NX_SHA256_LEN];
    nx_sha256_init(&c);
    nx_sha256_update(&c, pad, 64);
    nx_sha256_update(&c, msg, msg_len);
    nx_sha256_final(&c, inner);

    for (int i = 0; i < 64; i++)
        pad[i] = k[i] ^ 0x5c;
    nx_sha256_init(&c);
    nx_sha256_update(&c, pad, 64);
    nx_sha256_update(&c, inner, sizeof inner);
    nx_sha256_final(&c, out);

    memset(k, 0, sizeof k);
    memset(pad, 0, sizeof pad);
    memset(inner, 0, sizeof inner);
}

bool nx_ct_equal(const void *a, const void *b, size_t len)
{
    /* volatile stops the compiler from turning this into an early-exit memcmp. */
    const volatile uint8_t *x = a, *y = b;
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++)
        diff |= (uint8_t)(x[i] ^ y[i]);
    return diff == 0;
}
