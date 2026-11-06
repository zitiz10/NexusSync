/*
 * nx_server_util.h — shared by the server integration tests: a tiny raw
 * HTTP client over real sockets, and a fixture that runs the whole server
 * in-process (data dir, join code ABCD2345, a small web_root, an admin
 * "boss" signed in).
 */
#ifndef NX_SERVER_UTIL_H
#define NX_SERVER_UTIL_H

#include "api/app.h"
#include "api/auth.h"
#include "net/server.h"
#include "net/ws.h"
#include "nx_test.h"
#include "nx_test_util.h"

/* ---- a tiny client ------------------------------------------------------- */

typedef struct {
    int status;
    char head[8192];
    char body[65536];
    size_t body_len;
} resp;

static inline plat_sock dial(uint16_t port)
{
    plat_sock s;
    if (plat_tcp_connect("127.0.0.1", port, &s) != 0)
        return PLAT_SOCK_INVALID;
    plat_sock_set_timeouts(s, 5000, 5000);
    return s;
}

/* Reads one response (head + Content-Length body; a HEAD response has
 * the length of a body it does not send). 0 or -1. */
static inline int read_resp_ex(plat_sock s, resp *r, bool head_request)
{
    memset(r, 0, sizeof *r);
    size_t have = 0;
    char *end = NULL;
    while (!end) {
        if (have >= sizeof r->head - 1)
            return -1;
        int64_t n = plat_recv(s, r->head + have, 1); /* byte by byte: never read past the head */
        if (n <= 0)
            return -1;
        have += (size_t)n;
        r->head[have] = '\0';
        end = strstr(r->head, "\r\n\r\n");
    }
    if (sscanf(r->head, "HTTP/1.1 %d", &r->status) != 1)
        return -1;
    const char *cl = strstr(r->head, "Content-Length: ");
    size_t len = cl && !head_request ? (size_t)strtoull(cl + 16, NULL, 10) : 0;
    if (len >= sizeof r->body)
        return -1;
    while (r->body_len < len) {
        int64_t n = plat_recv(s, r->body + r->body_len, len - r->body_len);
        if (n <= 0)
            return -1;
        r->body_len += (size_t)n;
    }
    r->body[r->body_len] = '\0';
    return 0;
}

static inline int read_resp(plat_sock s, resp *r) { return read_resp_ex(s, r, false); }

/* One request on a fresh connection. extra: header lines ending in \r\n. */
static inline int req(uint16_t port, const char *method, const char *path, const char *extra,
               const char *body, resp *r)
{
    plat_sock s = dial(port);
    if (s == PLAT_SOCK_INVALID)
        return -1;
    char head[4096];
    size_t bl = body ? strlen(body) : 0;
    int n = snprintf(head, sizeof head,
                     "%s %s HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n%s%s%s", method,
                     path, extra ? extra : "", body ? "Content-Type: application/json\r\n" : "",
                     body ? "" : "\r\n");
    if (body)
        n += snprintf(head + n, sizeof head - (size_t)n, "Content-Length: %zu\r\n\r\n", bl);
    int rc = plat_send_all(s, head, (size_t)n);
    if (rc == 0 && bl)
        rc = plat_send_all(s, body, bl);
    if (rc == 0)
        rc = read_resp_ex(s, r, strcmp(method, "HEAD") == 0);
    plat_sock_close(s);
    return rc;
}

/* Sends raw bytes and reads one response. */
static inline int raw(uint16_t port, const char *bytes, size_t n, resp *r)
{
    plat_sock s = dial(port);
    if (s == PLAT_SOCK_INVALID)
        return -1;
    int rc = plat_send_all(s, bytes, n);
    if (rc == 0)
        rc = read_resp(s, r);
    plat_sock_close(s);
    return rc;
}

static inline bool cookie_of(const resp *r, char out[200])
{
    const char *c = strstr(r->head, "Set-Cookie: nx_session=");
    if (!c)
        return false;
    const char *e = strchr(c + 12, ';');
    size_t n = (size_t)(e - (c + 12));
    if (!e || n >= 200)
        return false;
    memcpy(out, c + 12, n);
    out[n] = '\0';
    return true;
}

/* ---- fixture -------------------------------------------------------------------- */

typedef struct {
    char dir[600];
    nx_app *app;
    nx_server *srv;
    uint16_t port;
    char admin_cookie[256]; /* "Cookie: nx_session=...\r\n" */
} fx;

static inline int write_text(const char *path, const char *text)
{
    plat_file *f;
    if (plat_file_open(path, PLAT_O_WRITE | PLAT_O_CREATE | PLAT_O_TRUNC, &f) != 0)
        return -1;
    int rc = plat_pwrite_all(f, text, strlen(text), 0);
    plat_file_close(f);
    return rc;
}

/* Data dir, join code ABCD2345, a small web_root, the server listening;
 * with open_engine also READY and an admin "boss" signed in. */
static inline int fx_start(fx *f, const char *name, bool open_engine, uint32_t workers, uint32_t max_conn)
{
    memset(f, 0, sizeof *f);
    nx_test_tmpdir(name, f->dir, sizeof f->dir);
    nx_config cfg;
    nx_config_defaults(&cfg);
    snprintf(cfg.data_dir, sizeof cfg.data_dir, "%s/data", f->dir);
    snprintf(cfg.web_root, sizeof cfg.web_root, "%s/web", f->dir);
    cfg.workers = workers;
    cfg.max_connections = max_conn;
    cfg.buffer_pool_pages = 256;
    char p[800];
    snprintf(p, sizeof p, "%s/web/assets", f->dir);
    if (nx_engine_create(cfg.data_dir) != 0 || nx_join_code_write(cfg.data_dir, "ABCD2345") != 0 ||
        plat_mkdir_p(p) != 0)
        return -1;
    snprintf(p, sizeof p, "%s/web/index.html", f->dir);
    write_text(p, "<!doctype html><title>NexusSync</title>");
    snprintf(p, sizeof p, "%s/web/assets/app-1a2b3c.js", f->dir);
    write_text(p, "console.log('hi')");
    snprintf(p, sizeof p, "%s/secret.txt", f->dir);
    write_text(p, "outside web_root");
    if (nx_app_create(&cfg, &f->app) != 0)
        return -1;
    f->app->pbkdf2_iters = 1000; /* tests only: keeps hundreds of logins fast */
    if (nx_server_start(f->app, "127.0.0.1", 0, &f->srv) != 0)
        return -1;
    f->port = nx_server_port(f->srv);
    if (!open_engine)
        return 0;
    if (nx_app_open_engine(f->app) != 0)
        return -1;
    nx_user u;
    memset(&u, 0, sizeof u);
    snprintf(u.username, sizeof u.username, "boss");
    snprintf(u.display, sizeof u.display, "Lab Admin");
    u.role = NX_ROLE_ADMIN;
    if (nx_password_set(&u, "admin-password-1", 1000) != 0 || nx_engine_add_user(f->app->engine, &u) != 0)
        return -1;
    resp r;
    if (req(f->port, "POST", "/api/auth/login", "X-NexusSync: 1\r\n",
            "{\"username\":\"boss\",\"password\":\"admin-password-1\"}", &r) != 0 ||
        r.status != 200)
        return -1;
    char ck[200];
    if (!cookie_of(&r, ck))
        return -1;
    snprintf(f->admin_cookie, sizeof f->admin_cookie, "Cookie: %s\r\n", ck);
    return 0;
}

static inline void fx_stop(fx *f)
{
    if (f->app)
        atomic_store(&f->app->state, NX_APP_STOPPING);
    nx_server_stop(f->srv);
    nx_app_destroy(f->app);
}

/* ---- WebSocket client (tests only) ---------------------------------------- */

static inline plat_sock ws_open(fx *f, const char *cookie_hdr, const char *extra, resp *r)
{
    plat_sock s = dial(f->port);
    if (s == PLAT_SOCK_INVALID)
        return s;
    char h[1024];
    int n = snprintf(h, sizeof h,
                     "GET /ws HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\n"
                     "Connection: keep-alive, Upgrade\r\nSec-WebSocket-Version: 13\r\n"
                     "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n%s%s\r\n",
                     cookie_hdr ? cookie_hdr : "", extra ? extra : "");
    if (plat_send_all(s, h, (size_t)n) != 0 || read_resp(s, r) != 0) {
        plat_sock_close(s);
        return PLAT_SOCK_INVALID;
    }
    return s;
}

/* Reads one server frame (unmasked). Returns the opcode or -1. */
static inline int ws_read(plat_sock s, char *payload, size_t cap, size_t *len)
{
    uint8_t h[10];
    if (plat_recv(s, h, 2) != 2)
        return -1;
    if (h[1] & 0x80)
        return -1; /* servers never mask */
    uint64_t n = h[1] & 0x7F;
    if (n == 126) {
        if (plat_recv(s, h + 2, 2) != 2)
            return -1;
        n = ((uint64_t)h[2] << 8) | h[3];
    }
    if (n >= cap)
        return -1;
    size_t got = 0;
    while (got < n) {
        int64_t k = plat_recv(s, payload + got, (size_t)n - got);
        if (k <= 0)
            return -1;
        got += (size_t)k;
    }
    payload[n] = '\0';
    *len = (size_t)n;
    return h[0] & 0x0F;
}

static inline int ws_send(plat_sock s, uint8_t opcode, const char *data, size_t n)
{
    uint8_t f[200];
    uint8_t mask[4] = {0x11, 0x22, 0x33, 0x44};
    f[0] = (uint8_t)(0x80 | opcode);
    f[1] = (uint8_t)(0x80 | n);
    memcpy(f + 2, mask, 4);
    for (size_t i = 0; i < n; i++)
        f[6 + i] = (uint8_t)data[i] ^ mask[i & 3];
    return plat_send_all(s, f, 6 + n);
}

/* Reads frames until a text frame containing needle (skips pings and
 * other events, at most 200 frames). The matching payload is copied to
 * out when given. */
static inline bool ws_wait_for_copy(plat_sock s, const char *needle, char *out, size_t cap)
{
    static char buf[16384];
    size_t len;
    for (int i = 0; i < 200; i++) {
        int op = ws_read(s, buf, sizeof buf, &len);
        if (op < 0)
            return false;
        if (op == NX_WS_TEXT && strstr(buf, needle)) {
            if (out && cap)
                snprintf(out, cap, "%s", buf);
            return true;
        }
    }
    return false;
}

static inline bool ws_wait_for(plat_sock s, const char *needle)
{
    return ws_wait_for_copy(s, needle, NULL, 0);
}

#endif
