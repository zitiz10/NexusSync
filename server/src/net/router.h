/*
 * router.h — one request's context and the dispatch table (SPEC §12).
 *
 * Every /api request passes the same gates, in order, before a handler
 * runs: state (503 while recovering), CSRF header on mutating methods,
 * body size limit, authentication, and role. Handlers then only do their
 * own job, and none can forget a check.
 */
#ifndef NX_ROUTER_H
#define NX_ROUTER_H

#include "api/app.h"
#include "net/http.h"
#include "util/json_out.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NX_BODY_JSON_MAX (1u << 20)   /* 1 MB */
#define NX_BODY_BINARY_MAX (8u << 20) /* 8 MB, upload manifest/chunks (M7) */

typedef struct {
    nx_app *app;
    plat_sock sock;
    nx_http_req *req;
    char path[2048]; /* percent-decoded */
    const char *peer_ip;
    const uint8_t *body;
    size_t body_len;
    bool keep_alive;
    bool head_only;
    /* Path parameters of the matched route. */
    uint64_t id;  /* {id}: decimal, 1..2^64-2 */
    char uid[33]; /* {uid}: 32 lower-case hex characters */
    /* Set by the auth gate. */
    bool authed;
    nx_user user;
    char session_token[65];
    /* Set by handlers. */
    bool upgraded;   /* the socket now belongs to the WebSocket hub */
    bool responded;
} nx_ctx;

/* Largest body the route for (method, path) accepts. */
size_t nx_route_body_limit(const char *method, const char *path);

/* Runs the request: static files, the API, or the WebSocket upgrade. */
void nx_route(nx_ctx *c);

/* Response helpers (set c->responded). */
int nx_reply_json(nx_ctx *c, int status, const nx_json *j, const char *extra_headers);
int nx_reply_error(nx_ctx *c, int status, const char *code, const char *message);
int nx_reply_empty(nx_ctx *c, int status, const char *extra_headers);

/* Writes the public user object {id, username, displayName, role}. */
void nx_json_user(nx_json *j, const nx_user *u);

#endif
