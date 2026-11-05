/*
 * sha256.h — SHA-256 (FIPS 180-4), written from the standard.
 * Used for block fingerprints, whole-file hashes, HMAC, and PBKDF2.
 * See docs/DECISIONS.md D-002 for why this is in-repo.
 */
#ifndef NX_SHA256_H
#define NX_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define NX_SHA256_LEN 32
#define NX_SHA256_BLOCK 64

typedef struct {
    uint32_t h[8];       /* chaining state */
    uint64_t total_len;  /* bytes hashed so far */
    uint8_t buf[64];     /* partial block */
    size_t buf_len;
} nx_sha256_ctx;

void nx_sha256_init(nx_sha256_ctx *c);
void nx_sha256_update(nx_sha256_ctx *c, const void *data, size_t len);
void nx_sha256_final(nx_sha256_ctx *c, uint8_t out[NX_SHA256_LEN]);
/* One-shot convenience. */
void nx_sha256(const void *data, size_t len, uint8_t out[NX_SHA256_LEN]);

/* Lowercase hex of a 32-byte digest into out (65 bytes incl. NUL). */
void nx_hex(const uint8_t *bytes, size_t n, char *out);
/* Parses 2*n hex chars into bytes. Returns 0 or -1 on bad input. */
int nx_unhex(const char *hex, uint8_t *out, size_t n);

/* Internal, exposed for HMAC/PBKDF2 precomputation: process one block. */
void nx_sha256_compress(uint32_t h[8], const uint8_t block[64]);

#endif
