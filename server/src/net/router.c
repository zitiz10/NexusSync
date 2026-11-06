/* router.c — request dispatch and gates. See router.h. */
#include "net/router.h"

#include "api/admin.h"
#include "api/auth.h"
#include "api/compare.h"
#include "api/files.h"
#include "api/stats.h"
#include "api/uploads.h"
#include "nexus_version.h"
#include "net/static.h"
#include "util/log.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* ===== responses ============================================================= */

int nx_reply_json(nx_ctx *c, int status, const nx_json *j, const char *extra)
{
    c->responded = true;
    if (j->oom)
        return nx_http_send_error(c->sock, 500, "internal", "Out of memory.", false);
    return nx_http_send(c->sock, status, "application/json; charset=utf-8", extra, j->buf, j->len,
                        c->keep_alive, c->head_only);
}

int nx_reply_error(nx_ctx *c, int status, const char *code, const char *message)
{
    c->responded = true;
    return nx_http_send_error(c->sock, status, code, message, c->keep_alive);
}

int nx_reply_empty(nx_ctx *c, int status, const char *extra)
{
    c->responded = true;
    return nx_http_send(c->sock, status, NULL, extra, NULL, 0, c->keep_alive, true);
}

void nx_json_user(nx_json *j, const nx_user *u)
{
    nx_json_obj_open(j);
    nx_json_key_u64(j, "id", u->id);
    nx_json_key_str(j, "username", u->username);
    nx_json_key_str(j, "displayName", u->display);
    nx_json_key_str(j, "role", u->role == NX_ROLE_ADMIN ? "admin" : "student");
    nx_json_obj_close(j);
}

/* ===== small handlers ============================================================ */

static void h_health(nx_ctx *c)
{
    int st = atomic_load(&c->app->state);
    const char *s = st == NX_APP_READY ? "ok"
                    : st == NX_APP_RECOVERING ? "recovering"
                    : st == NX_APP_STOPPING ? "stopping" : "failed";
    nx_json j;
    nx_json_init(&j);
    nx_json_obj_open(&j);
    nx_json_key_str(&j, "status", s);
    nx_json_key_str(&j, "version", NX_VERSION);
    if (st == NX_APP_RECOVERING) {
        nx_json_key(&j, "progress");
        nx_json_obj_open(&j);
        nx_json_key_str(&j, "phase", "replaying-log");
        nx_json_key_u64(&j, "elapsedMs", plat_now_mono_ms() - c->app->started_ms);
        nx_json_key_u64(&j, "recordsReplayed", atomic_load(&c->app->replayed));
        nx_json_obj_close(&j);
    }
    nx_json_obj_close(&j);
    nx_reply_json(c, st == NX_APP_FAILED ? 503 : 200, &j, "Cache-Control: no-store\r\n");
    nx_json_free(&j);
}

static void h_admin_checkpoint(nx_ctx *c)
{
    if (nx_engine_checkpoint(c->app->engine) != 0) {
        nx_reply_error(c, 500, "checkpoint_failed", "The checkpoint failed; see the server log.");
        return;
    }
    nx_engine_stats st;
    nx_engine_get_stats(c->app->engine, &st);
    nx_json j;
    nx_json_init(&j);
    nx_json_obj_open(&j);
    nx_json_key_u64(&j, "lsn", st.checkpoint_lsn);
    nx_json_key_u64(&j, "pages", st.last_checkpoint_pages);
    nx_json_key_u64(&j, "durationMs", st.last_checkpoint_ms);
    nx_json_obj_close(&j);
    nx_reply_json(c, 200, &j, NULL);
    nx_json_free(&j);
}

/* "upgrade" in a comma-separated Connection header, any case. */
static bool has_token(const char *v, const char *tok)
{
    size_t tl = strlen(tok);
    for (const char *p = v; p && *p;) {
        while (*p == ' ' || *p == ',')
            p++;
        const char *e = p;
        while (*e && *e != ',')
            e++;
        const char *t = e;
        while (t > p && t[-1] == ' ')
            t--;
        if ((size_t)(t - p) == tl) {
            bool same = true;
            for (size_t i = 0; i < tl; i++) {
                char a = p[i] >= 'A' && p[i] <= 'Z' ? (char)(p[i] + 32) : p[i];
                if (a != tok[i])
                    same = false;
            }
            if (same)
                return true;
        }
        p = e;
    }
    return false;
}

/* The Origin's host[:port] must equal Host: stops other web pages from
 * opening a socket with this user's cookie (cross-site WebSocket hijack). */
static bool origin_ok(const nx_http_req *r)
{
    const char *origin = nx_http_get_header(r, "origin"), *host = nx_http_get_header(r, "host");
    if (!origin)
        return true; /* non-browser clients (CLI, tests) send none */
    const char *sep = strstr(origin, "://");
    return sep && host && strcmp(sep + 3, host) == 0;
}

static void h_ws(nx_ctx *c)
{
    const nx_http_req *r = c->req;
    const char *up = nx_http_get_header(r, "upgrade"), *conn = nx_http_get_header(r, "connection"),
               *ver = nx_http_get_header(r, "sec-websocket-version"),
               *key = nx_http_get_header(r, "sec-websocket-key");
    if (strcmp(r->method, "GET") != 0 || r->minor != 1 || !up || !has_token(up, "websocket") ||
        !conn || !has_token(conn, "upgrade")) {
        nx_reply_error(c, 400, "bad_upgrade", "Expected a WebSocket upgrade request.");
        return;
    }
    if (!ver || strcmp(ver, "13") != 0) {
        c->responded = true;
        nx_http_send(c->sock, 426, "application/json; charset=utf-8",
                     "Sec-WebSocket-Version: 13\r\n", "{\"error\":{\"code\":\"bad_version\",\"message\":\"Use WebSocket version 13.\"}}",
                     strlen("{\"error\":{\"code\":\"bad_version\",\"message\":\"Use WebSocket version 13.\"}}"),
                     false, false);
        c->keep_alive = false;
        return;
    }
    char accept[29];
    if (nx_ws_accept_key(key, accept) != 0) {
        nx_reply_error(c, 400, "bad_key", "Missing or malformed Sec-WebSocket-Key.");
        return;
    }
    if (!origin_ok(r)) {
        nx_reply_error(c, 403, "bad_origin", "WebSocket connections must come from this site.");
        return;
    }
    if (!c->authed) {
        nx_reply_error(c, 401, "unauthorized", "Sign in first.");
        return;
    }
    char resp[256];
    int n = snprintf(resp, sizeof resp,
                     "HTTP/1.1 101 Switching Protocols\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Accept: %s\r\n\r\n",
                     accept);
    c->responded = true;
    if (plat_send_all(c->sock, resp, (size_t)n) != 0) {
        c->keep_alive = false;
        return;
    }
    c->upgraded = true; /* the hub owns the socket from here, even on failure */
    if (nx_hub_add(c->app->hub, c->sock, c->user.id, c->user.role == NX_ROLE_ADMIN) != 0)
        LOG_WARN("ws", "connection refused: hub full or stopping");
}

/* ===== table ======================================================================== */

/* Matches path against a pattern whose segments may be {id} or {uid}.
 * Parameters are validated here, so handlers get only well-formed ids. */
static bool match(const char *pattern, const char *path, nx_ctx *c)
{
    const char *p = pattern, *s = path;
    while (*p && *s) {
        if (strncmp(p, "{id}", 4) == 0) {
            uint64_t v = 0;
            size_t n = 0;
            for (; s[n] >= '0' && s[n] <= '9'; n++) {
                if (n >= 19 || (n == 0 && s[n] == '0'))
                    return false; /* too long, or a leading zero */
                v = v * 10 + (uint64_t)(s[n] - '0');
            }
            if (n == 0 || v == 0)
                return false;
            c->id = v;
            p += 4;
            s += n;
        } else if (strncmp(p, "{uid}", 5) == 0) {
            size_t n = 0;
            for (; (s[n] >= '0' && s[n] <= '9') || (s[n] >= 'a' && s[n] <= 'f'); n++)
                if (n >= 32)
                    return false;
            if (n != 32)
                return false;
            memcpy(c->uid, s, 32);
            c->uid[32] = '\0';
            p += 5;
            s += 32;
        } else if (*p == *s) {
            p++;
            s++;
        } else {
            return false;
        }
    }
    return *p == '\0' && *s == '\0';
}

enum { R_AUTH = 1, R_ADMIN = 2, R_ANY_STATE = 4 };

typedef struct {
    const char *method;
    const char *path;
    void (*fn)(nx_ctx *);
    unsigned flags;
} route;

static const route ROUTES[] = {
    {"GET", "/api/health", h_health, R_ANY_STATE},
    {"POST", "/api/auth/join", nx_h_join, 0},
    {"POST", "/api/auth/login", nx_h_login, 0},
    {"POST", "/api/auth/logout", nx_h_logout, 0},
    {"GET", "/api/me", nx_h_me, R_AUTH},
    {"GET", "/api/admin/join-code", nx_h_admin_join_code, R_ADMIN},
    {"POST", "/api/admin/join-code/rotate", nx_h_admin_join_code_rotate, R_ADMIN},
    {"POST", "/api/admin/checkpoint", h_admin_checkpoint, R_ADMIN},
    {"GET", "/api/admin/users", nx_h_admin_users, R_ADMIN},
    {"POST", "/api/admin/users/{id}", nx_h_admin_user_update, R_ADMIN},
    {"GET", "/api/admin/events", nx_h_admin_events, R_ADMIN},
    {"GET", "/api/admin/recoveries", nx_h_admin_recoveries, R_ADMIN},
    {"GET", "/api/admin/addresses", nx_h_admin_addresses, R_ADMIN},
    {"POST", "/api/admin/gc", nx_h_admin_gc, R_ADMIN},
    {"POST", "/api/admin/scan", nx_h_admin_scan, R_ADMIN},
    {"GET", "/api/stats", nx_h_stats, R_AUTH},
    {"GET", "/api/blockmap", nx_h_blockmap, R_AUTH},
    {"GET", "/api/compare", nx_h_compare, R_AUTH},
    {"GET", "/api/files", nx_h_files_list, R_AUTH},
    {"GET", "/api/files/{id}", nx_h_file_get, R_AUTH},
    {"PATCH", "/api/files/{id}", nx_h_file_patch, R_AUTH},
    {"DELETE", "/api/files/{id}", nx_h_file_delete, R_AUTH},
    {"GET", "/api/files/{id}/content", nx_h_file_content, R_AUTH},
    {"POST", "/api/uploads", nx_h_upload_create, R_AUTH},
    {"POST", "/api/uploads/{uid}/manifest", nx_h_upload_manifest, R_AUTH},
    {"POST", "/api/uploads/{uid}/chunks", nx_h_upload_chunks, R_AUTH},
    {"POST", "/api/uploads/{uid}/commit", nx_h_upload_commit, R_AUTH},
    {"DELETE", "/api/uploads/{uid}", nx_h_upload_cancel, R_AUTH},
};

size_t nx_route_body_limit(const char *method, const char *path)
{
    (void)method;
    /* M7 adds the binary upload routes (manifest, chunks) at 8 MB. */
    if (strncmp(path, "/api/uploads/", 13) == 0 &&
        (strstr(path, "/manifest") || strstr(path, "/chunks")))
        return NX_BODY_BINARY_MAX;
    return NX_BODY_JSON_MAX;
}

static bool is_mutating(const char *m)
{
    return strcmp(m, "POST") == 0 || strcmp(m, "PUT") == 0 || strcmp(m, "PATCH") == 0 ||
           strcmp(m, "DELETE") == 0;
}

void nx_route(nx_ctx *c)
{
    int state = atomic_load(&c->app->state);
    bool ready = state == NX_APP_READY;
    if (strcmp(c->path, "/ws") == 0) {
        if (!ready) {
            nx_reply_error(c, 503, "unavailable", "The server is starting or stopping.");
            return;
        }
        nx_auth_from_cookie(c);
        h_ws(c);
        return;
    }
    if (strncmp(c->path, "/api/", 5) != 0 && strcmp(c->path, "/api") != 0) {
        /* The web app is served even while recovering, so it can show the
         * recovering state instead of a browser error page. */
        nx_static_serve(c);
        return;
    }
    const route *r = NULL;
    bool path_known = false;
    bool head = strcmp(c->req->method, "HEAD") == 0;
    for (size_t i = 0; i < sizeof ROUTES / sizeof ROUTES[0]; i++) {
        if (!match(ROUTES[i].path, c->path, c))
            continue;
        path_known = true;
        if (strcmp(ROUTES[i].method, c->req->method) == 0 ||
            (head && strcmp(ROUTES[i].method, "GET") == 0))
            r = &ROUTES[i];
    }
    if (!r) {
        if (path_known)
            nx_reply_error(c, 405, "method_not_allowed", "That method is not allowed here.");
        else
            nx_reply_error(c, 404, "not_found", "No such endpoint.");
        return;
    }
    if (!ready && !(r->flags & R_ANY_STATE)) {
        c->responded = true;
        nx_http_send(c->sock, 503, "application/json; charset=utf-8", "Retry-After: 5\r\n",
                     "{\"error\":{\"code\":\"recovering\",\"message\":\"The server is starting up. Try again in a moment.\"}}",
                     strlen("{\"error\":{\"code\":\"recovering\",\"message\":\"The server is starting up. Try again in a moment.\"}}"),
                     c->keep_alive, c->head_only);
        return;
    }
    /* CSRF guard (SPEC §12): a cross-site form or fetch cannot set this
     * header without a CORS preflight, which we never approve. */
    if (is_mutating(c->req->method)) {
        const char *x = nx_http_get_header(c->req, "x-nexussync");
        if (!x || strcmp(x, "1") != 0) {
            nx_reply_error(c, 403, "csrf", "Missing X-NexusSync header.");
            return;
        }
    }
    if (ready)
        nx_auth_from_cookie(c);
    if ((r->flags & (R_AUTH | R_ADMIN)) && !c->authed) {
        nx_reply_error(c, 401, "unauthorized", "Sign in first.");
        return;
    }
    if ((r->flags & R_ADMIN) && c->user.role != NX_ROLE_ADMIN) {
        nx_reply_error(c, 403, "forbidden", "Only the lab admin can do that.");
        return;
    }
    r->fn(c);
    if (!c->responded)
        nx_reply_error(c, 500, "internal", "No response was produced.");
}
