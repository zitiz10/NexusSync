/*
 * client.h — a small HTTP/1.1 client for the nexus CLI.
 *
 * One keep-alive connection, reused between requests. The server closes
 * idle connections after 5 s, so a connection idle for 3 s is replaced
 * before use; a request that fails before any response byte arrives on a
 * reused connection is retried once on a fresh one.
 */
#ifndef NX_CLIENT_H
#define NX_CLIENT_H

#include "platform/platform.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char host[256];
    uint16_t port;
    char token[80]; /* session token, "" when signed out */
    plat_sock s;
    bool connected;
    uint64_t last_used_ms;
} nx_client;

typedef struct {
    int status;
    char head[16384];
    char *body; /* malloc'd, NUL-terminated (NULL when streamed) */
    size_t body_len;
} nx_response;

/* Streams the response body; return non-zero to abort. */
typedef int (*nx_body_cb)(const uint8_t *data, size_t len, void *ctx);

/* "http://host:port" (port defaults to 8080). */
int nx_client_init(nx_client *c, const char *url);
void nx_client_close(nx_client *c);

/* Sends one request. ctype may be NULL. When sink is given the body goes
 * to it instead of r->body. Returns 0 if a response was received (any
 * status), -1 on network failure. */
int nx_client_request(nx_client *c, const char *method, const char *path, const char *ctype,
                      const void *body, size_t len, nx_body_cb sink, void *ctx, nx_response *r);
void nx_response_free(nx_response *r);

/* The nx_session value of a Set-Cookie header in r, if any. */
bool nx_response_cookie(const nx_response *r, char *out, size_t cap);

#endif
