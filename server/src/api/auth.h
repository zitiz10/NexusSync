/*
 * auth.h — accounts, sessions, and login rate limits (SPEC §13).
 *
 * Sessions: the browser holds a random 32-byte token (hex, in the
 * HttpOnly nx_session cookie). The server keeps only SHA-256(token) ->
 * {user, expiry} in memory, so even a memory dump does not reveal usable
 * tokens. A restart signs everyone out (documented).
 *
 * Rate limits: 5 failed logins per username and 20 per IP address in any
 * 60 s window. While a key is over its limit the request is refused with
 * 429 *before* the password is hashed, which also stops anyone from
 * burning the server's CPU with PBKDF2.
 */
#ifndef NX_AUTH_H
#define NX_AUTH_H

#include "api/app.h"
#include "net/router.h"

#include <stdbool.h>
#include <stdint.h>

#define NX_LOGIN_FAILS_PER_USER 5
#define NX_LOGIN_FAILS_PER_IP 20
#define NX_PASSWORD_MIN 10
#define NX_PASSWORD_MAX 256

int nx_auth_create(uint32_t session_ttl_s, nx_auth **out);
void nx_auth_destroy(nx_auth *a);

/* token_hex receives 64 hex characters + NUL. */
int nx_session_create(nx_auth *a, uint32_t user_id, char token_hex[65]);
bool nx_session_lookup(nx_auth *a, const char *token_hex, uint32_t *user_id);
void nx_session_delete(nx_auth *a, const char *token_hex);
void nx_session_delete_user(nx_auth *a, uint32_t user_id);
size_t nx_session_count(nx_auth *a);

/* Rate limiting; now_ms is passed in so tests can move time. */
bool nx_rl_blocked(nx_auth *a, const char *username, const char *ip, uint64_t now_ms);
void nx_rl_fail(nx_auth *a, const char *username, const char *ip, uint64_t now_ms);

/* Fills salt (random), iterations, and pw_hash of u. */
int nx_password_set(nx_user *u, const char *password, uint32_t iterations);
bool nx_password_check(const nx_user *u, const char *password);

/* Lower-cases and checks a username: 3..32 of [a-z0-9._-], starting with
 * a letter or digit. */
bool nx_username_normalize(const char *in, char out[NX_USERNAME_MAX + 1]);
/* 1..64 bytes of valid UTF-8 without control characters. */
bool nx_display_valid(const char *s);
bool nx_utf8_valid(const char *s, size_t n);

/* Handlers (router.c dispatches to them). */
void nx_h_join(nx_ctx *c);
void nx_h_login(nx_ctx *c);
void nx_h_logout(nx_ctx *c);
void nx_h_me(nx_ctx *c);
void nx_h_admin_join_code(nx_ctx *c);
void nx_h_admin_join_code_rotate(nx_ctx *c);

/* Auth gate: reads the session cookie and loads the user. False when there
 * is no valid session (or the account is disabled). */
bool nx_auth_from_cookie(nx_ctx *c);

#endif
