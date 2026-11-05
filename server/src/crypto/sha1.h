/*
 * sha1.h — SHA-1 (FIPS 180-4 §6.1). Used ONLY for the WebSocket handshake
 * (RFC 6455 requires it). SHA-1 is broken for collision resistance, so
 * never use it for fingerprints or passwords.
 */
#ifndef NX_SHA1_H
#define NX_SHA1_H

#include <stddef.h>
#include <stdint.h>

#define NX_SHA1_LEN 20

typedef struct {
    uint32_t h[5];
    uint64_t total_len;
    uint8_t buf[64];
    size_t buf_len;
} nx_sha1_ctx;

void nx_sha1_init(nx_sha1_ctx *c);
void nx_sha1_update(nx_sha1_ctx *c, const void *data, size_t len);
void nx_sha1_final(nx_sha1_ctx *c, uint8_t out[NX_SHA1_LEN]);
void nx_sha1(const void *data, size_t len, uint8_t out[NX_SHA1_LEN]);

#endif
