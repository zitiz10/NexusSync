/*
 * kdf.h — HMAC-SHA256 (RFC 2104), PBKDF2-HMAC-SHA256 (RFC 8018), and a
 * constant-time comparison. Used for password hashing (SPEC §13).
 */
#ifndef NX_KDF_H
#define NX_KDF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "crypto/sha256.h"

void nx_hmac_sha256(const void *key, size_t key_len, const void *msg, size_t msg_len,
                    uint8_t out[NX_SHA256_LEN]);

/* Derives out_len bytes. iterations >= 1. */
void nx_pbkdf2_sha256(const void *password, size_t password_len, const void *salt,
                      size_t salt_len, uint32_t iterations, uint8_t *out, size_t out_len);

/* Compares in time that depends only on len, not on where bytes differ,
 * so an attacker cannot learn a hash byte by byte from response times. */
bool nx_ct_equal(const void *a, const void *b, size_t len);

#endif
