/*
 * test_server.c — the HTTP server end to end over real sockets (M6):
 *   - health while recovering (503 elsewhere) and when ready
 *   - static files: index, hashed assets, SPA fallback, traversal refused
 *   - malformed and oversized input over the wire: 400, 411, 413, 415, 431
 *   - keep-alive and pipelined requests on one connection
 *   - join (bad code, weak password, taken name), login, me, logout,
 *     CSRF header, admin-only routes, rate limits (per user and per IP)
 *   - WebSocket: handshake rules, an event delivered (engine.checkpoint),
 *     ping/pong, close, server.shutdown on stop
 *   - a full server answers 503
 */
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

static plat_sock dial(uint16_t port)
{
    plat_sock s;
    if (plat_tcp_connect("127.0.0.1", port, &s) != 0)
        return PLAT_SOCK_INVALID;
    plat_sock_set_timeouts(s, 5000, 5000);
    return s;
}

/* Reads one response (head + Content-Length body; a HEAD response has
 * the length of a body it does not send). 0 or -1. */
static int read_resp_ex(plat_sock s, resp *r, bool head_request)
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

static int read_resp(plat_sock s, resp *r) { return read_resp_ex(s, r, false); }

/* One request on a fresh connection. extra: header lines ending in \r\n. */
static int req(uint16_t port, const char *method, const char *path, const char *extra,
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
static int raw(uint16_t port, const char *bytes, size_t n, resp *r)
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

static bool cookie_of(const resp *r, char out[200])
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

static int write_text(const char *path, const char *text)
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
static int fx_start(fx *f, const char *name, bool open_engine, uint32_t workers, uint32_t max_conn)
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

static void fx_stop(fx *f)
{
    if (f->app)
        atomic_store(&f->app->state, NX_APP_STOPPING);
    nx_server_stop(f->srv);
    nx_app_destroy(f->app);
}

/* ---- tests ------------------------------------------------------------------------- */

static void test_recovering_then_ready(void)
{
    fx f;
    REQUIRE(fx_start(&f, "srv_recovering", false, 2, 16) == 0);
    resp r;
    REQUIRE(req(f.port, "GET", "/api/health", NULL, NULL, &r) == 0);
    CHECK(r.status == 200 && strstr(r.body, "\"status\":\"recovering\"") &&
          strstr(r.body, "\"progress\""));
    REQUIRE(req(f.port, "GET", "/api/me", NULL, NULL, &r) == 0);
    CHECK(r.status == 503 && strstr(r.head, "Retry-After: 5") && strstr(r.body, "recovering"));
    REQUIRE(req(f.port, "GET", "/", NULL, NULL, &r) == 0); /* the app itself still loads */
    CHECK(r.status == 200 && strstr(r.body, "NexusSync"));
    REQUIRE(nx_app_open_engine(f.app) == 0);
    REQUIRE(req(f.port, "GET", "/api/health", NULL, NULL, &r) == 0);
    CHECK(r.status == 200 && strstr(r.body, "\"status\":\"ok\"") && !strstr(r.body, "progress"));
    fx_stop(&f);
}

static void test_static_files(void)
{
    fx f;
    REQUIRE(fx_start(&f, "srv_static", true, 2, 16) == 0);
    resp r;
    REQUIRE(req(f.port, "GET", "/", NULL, NULL, &r) == 0);
    CHECK(r.status == 200 && strstr(r.head, "Cache-Control: no-cache") &&
          strstr(r.head, "text/html") && strstr(r.head, "X-Frame-Options: DENY"));
    REQUIRE(req(f.port, "GET", "/assets/app-1a2b3c.js", NULL, NULL, &r) == 0);
    CHECK(r.status == 200 && strstr(r.head, "immutable") && strstr(r.head, "text/javascript") &&
          strcmp(r.body, "console.log('hi')") == 0);
    REQUIRE(req(f.port, "HEAD", "/assets/app-1a2b3c.js", NULL, NULL, &r) == 0);
    CHECK(r.status == 200 && r.body_len == 0); /* read_resp stops: no body bytes */
    REQUIRE(req(f.port, "GET", "/files/42?tab=versions", NULL, NULL, &r) == 0);
    CHECK(r.status == 200 && strstr(r.body, "NexusSync")); /* client-side route */
    REQUIRE(req(f.port, "GET", "/assets/missing.js", NULL, NULL, &r) == 0);
    CHECK(r.status == 404);
    REQUIRE(req(f.port, "GET", "/favicon.ico", NULL, NULL, &r) == 0);
    CHECK(r.status == 404);
    const char *bad[] = {"/../secret.txt", "/%2e%2e/secret.txt", "/assets/..%2f..%2fsecret.txt",
                         "/.hidden", "/a%5c..%5csecret.txt", "/%00"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        REQUIRE(req(f.port, "GET", bad[i], NULL, NULL, &r) == 0);
        CHECK(r.status == 400);
        CHECK(!strstr(r.body, "outside web_root"));
    }
    REQUIRE(req(f.port, "POST", "/index.html", "X-NexusSync: 1\r\n", "{}", &r) == 0);
    CHECK(r.status == 405);
    REQUIRE(req(f.port, "GET", "/api/nope", NULL, NULL, &r) == 0);
    CHECK(r.status == 404 && strstr(r.body, "\"code\":\"not_found\""));
    REQUIRE(req(f.port, "DELETE", "/api/health", "X-NexusSync: 1\r\n", NULL, &r) == 0);
    CHECK(r.status == 405);
    fx_stop(&f);
}

static void test_malformed_over_the_wire(void)
{
    fx f;
    REQUIRE(fx_start(&f, "srv_malformed", true, 2, 16) == 0);
    resp r;
    const char *g = "NOT HTTP AT ALL\r\n\r\n";
    REQUIRE(raw(f.port, g, strlen(g), &r) == 0);
    CHECK(r.status == 400 && strstr(r.head, "Connection: close"));
    const char *ch = "POST /api/auth/login HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n";
    REQUIRE(raw(f.port, ch, strlen(ch), &r) == 0);
    CHECK(r.status == 411);
    char *big = malloc(20000);
    REQUIRE(big);
    int n = snprintf(big, 20000, "GET / HTTP/1.1\r\nHost: h\r\nX-Big: ");
    memset(big + n, 'x', 19000 - (size_t)n);
    REQUIRE(raw(f.port, big, 19000, &r) == 0);
    CHECK(r.status == 431);
    free(big);
    /* A 2 MB body to a JSON endpoint is refused before it is read. */
    const char *huge = "POST /api/auth/login HTTP/1.1\r\nHost: h\r\nX-NexusSync: 1\r\n"
                       "Content-Type: application/json\r\nContent-Length: 2097152\r\n\r\n{";
    REQUIRE(raw(f.port, huge, strlen(huge), &r) == 0);
    CHECK(r.status == 413);
    REQUIRE(req(f.port, "POST", "/api/auth/login", "X-NexusSync: 1\r\n", "{not json", &r) == 0);
    CHECK(r.status == 400 && strstr(r.body, "invalid_json"));
    REQUIRE(req(f.port, "POST", "/api/auth/login", "X-NexusSync: 1\r\n", "[1,2]", &r) == 0);
    CHECK(r.status == 400);
    const char *form = "POST /api/auth/login HTTP/1.1\r\nHost: h\r\nX-NexusSync: 1\r\n"
                       "Content-Type: text/plain\r\nContent-Length: 2\r\n\r\n{}";
    REQUIRE(raw(f.port, form, strlen(form), &r) == 0);
    CHECK(r.status == 415);
    /* The server is still fine afterwards. */
    REQUIRE(req(f.port, "GET", "/api/health", NULL, NULL, &r) == 0);
    CHECK(r.status == 200);
    fx_stop(&f);
}

static void test_keep_alive_and_pipelining(void)
{
    fx f;
    REQUIRE(fx_start(&f, "srv_keepalive", true, 2, 16) == 0);
    plat_sock s = dial(f.port);
    REQUIRE(s != PLAT_SOCK_INVALID);
    const char *one = "GET /api/health HTTP/1.1\r\nHost: h\r\n\r\n";
    resp r;
    for (int i = 0; i < 3; i++) {
        REQUIRE(plat_send_all(s, one, strlen(one)) == 0);
        REQUIRE(read_resp(s, &r) == 0);
        CHECK(r.status == 200 && strstr(r.head, "Connection: keep-alive"));
    }
    /* Two requests in one write, the second with a body. */
    char two[512];
    int n = snprintf(two, sizeof two,
                     "GET /api/health HTTP/1.1\r\nHost: h\r\n\r\n"
                     "POST /api/auth/login HTTP/1.1\r\nHost: h\r\nX-NexusSync: 1\r\n"
                     "Content-Type: application/json\r\nContent-Length: 2\r\n\r\n{}");
    REQUIRE(plat_send_all(s, two, (size_t)n) == 0);
    REQUIRE(read_resp(s, &r) == 0);
    CHECK(r.status == 200);
    REQUIRE(read_resp(s, &r) == 0);
    CHECK(r.status == 400 && strstr(r.body, "missing_fields"));
    plat_sock_close(s);
    fx_stop(&f);
}

static void test_join_login_logout(void)
{
    fx f;
    REQUIRE(fx_start(&f, "srv_auth", true, 4, 16) == 0);
    const char *H = "X-NexusSync: 1\r\n";
    resp r;
    REQUIRE(req(f.port, "POST", "/api/auth/join", H,
                "{\"username\":\"riya\",\"displayName\":\"Riya K\",\"password\":\"long-enough-1\",\"joinCode\":\"WRONG234\"}",
                &r) == 0);
    CHECK(r.status == 403 && strstr(r.body, "bad_join_code"));
    REQUIRE(req(f.port, "POST", "/api/auth/join", H,
                "{\"username\":\"riya\",\"displayName\":\"Riya K\",\"password\":\"short\",\"joinCode\":\"ABCD2345\"}",
                &r) == 0);
    CHECK(r.status == 400 && strstr(r.body, "weak_password"));
    REQUIRE(req(f.port, "POST", "/api/auth/join", H,
                "{\"username\":\"x\",\"displayName\":\"Riya K\",\"password\":\"long-enough-1\",\"joinCode\":\"ABCD2345\"}",
                &r) == 0);
    CHECK(r.status == 400 && strstr(r.body, "invalid_username"));
    /* No CSRF header: refused even with a good body. */
    REQUIRE(req(f.port, "POST", "/api/auth/join", NULL,
                "{\"username\":\"riya\",\"displayName\":\"Riya K\",\"password\":\"long-enough-1\",\"joinCode\":\"ABCD2345\"}",
                &r) == 0);
    CHECK(r.status == 403 && strstr(r.body, "csrf"));
    /* The code is case-insensitive; the username is stored lower-case. */
    REQUIRE(req(f.port, "POST", "/api/auth/join", H,
                "{\"username\":\"Riya\",\"displayName\":\"Riya K\",\"password\":\"long-enough-1\",\"joinCode\":\"abcd2345\"}",
                &r) == 0);
    CHECK(r.status == 201 && strstr(r.body, "\"username\":\"riya\"") &&
          strstr(r.body, "\"role\":\"student\""));
    char ck[200], hdr[300];
    REQUIRE(cookie_of(&r, ck));
    CHECK(strstr(r.head, "HttpOnly; SameSite=Strict; Path=/; Max-Age=43200"));
    snprintf(hdr, sizeof hdr, "Cookie: %s\r\n", ck);
    REQUIRE(req(f.port, "POST", "/api/auth/join", H,
                "{\"username\":\"riya\",\"displayName\":\"Other\",\"password\":\"long-enough-1\",\"joinCode\":\"ABCD2345\"}",
                &r) == 0);
    CHECK(r.status == 409);

    REQUIRE(req(f.port, "GET", "/api/me", hdr, NULL, &r) == 0);
    CHECK(r.status == 200 && strstr(r.body, "\"displayName\":\"Riya K\""));
    REQUIRE(req(f.port, "GET", "/api/me", NULL, NULL, &r) == 0);
    CHECK(r.status == 401);
    /* A student is not an admin. */
    REQUIRE(req(f.port, "GET", "/api/admin/join-code", hdr, NULL, &r) == 0);
    CHECK(r.status == 403);
    REQUIRE(req(f.port, "GET", "/api/admin/join-code", f.admin_cookie, NULL, &r) == 0);
    CHECK(r.status == 200 && strstr(r.body, "ABCD2345"));
    char hdr2[600];
    snprintf(hdr2, sizeof hdr2, "%s%s", f.admin_cookie, H);
    REQUIRE(req(f.port, "POST", "/api/admin/join-code/rotate", hdr2, NULL, &r) == 0);
    CHECK(r.status == 200 && !strstr(r.body, "ABCD2345"));
    char newcode[16] = "";
    const char *jc = strstr(r.body, "\"joinCode\":\"");
    if (jc)
        memcpy(newcode, jc + 12, 8);
    REQUIRE(req(f.port, "POST", "/api/auth/join", H,
                "{\"username\":\"kabir\",\"displayName\":\"Kabir\",\"password\":\"long-enough-1\",\"joinCode\":\"ABCD2345\"}",
                &r) == 0);
    CHECK(r.status == 403); /* the old code stopped working */
    char join_new[300];
    snprintf(join_new, sizeof join_new,
             "{\"username\":\"kabir\",\"displayName\":\"Kabir\",\"password\":\"long-enough-1\",\"joinCode\":\"%s\"}",
             newcode);
    REQUIRE(req(f.port, "POST", "/api/auth/join", H, join_new, &r) == 0);
    CHECK(r.status == 201);

    /* Login: wrong password, then right; logout ends the session. */
    REQUIRE(req(f.port, "POST", "/api/auth/login", H,
                "{\"username\":\"riya\",\"password\":\"wrong-password\"}", &r) == 0);
    CHECK(r.status == 401 && !strstr(r.head, "Set-Cookie"));
    REQUIRE(req(f.port, "POST", "/api/auth/login", H,
                "{\"username\":\"RIYA\",\"password\":\"long-enough-1\"}", &r) == 0);
    CHECK(r.status == 200);
    char ck2[200], hdr3[600];
    REQUIRE(cookie_of(&r, ck2));
    snprintf(hdr3, sizeof hdr3, "Cookie: %s\r\n%s", ck2, H);
    REQUIRE(req(f.port, "POST", "/api/auth/logout", hdr3, NULL, &r) == 0);
    CHECK(r.status == 204 && strstr(r.head, "Max-Age=0"));
    snprintf(hdr3, sizeof hdr3, "Cookie: %s\r\n", ck2);
    REQUIRE(req(f.port, "GET", "/api/me", hdr3, NULL, &r) == 0);
    CHECK(r.status == 401);
    REQUIRE(req(f.port, "GET", "/api/me", hdr, NULL, &r) == 0); /* the first session lives on */
    CHECK(r.status == 200);

    /* A disabled account can no longer use its session or sign in. */
    nx_user u;
    REQUIRE(nx_catalog_user_by_name(nx_engine_catalog(f.app->engine), "riya", &u));
    u.status = NX_STATUS_DISABLED;
    REQUIRE(nx_engine_update_user(f.app->engine, &u) == 0);
    REQUIRE(req(f.port, "GET", "/api/me", hdr, NULL, &r) == 0);
    CHECK(r.status == 401);
    REQUIRE(req(f.port, "POST", "/api/auth/login", H,
                "{\"username\":\"riya\",\"password\":\"long-enough-1\"}", &r) == 0);
    CHECK(r.status == 403 && strstr(r.body, "account_disabled"));
    fx_stop(&f);
}

static void test_rate_limits(void)
{
    fx f;
    REQUIRE(fx_start(&f, "srv_rate", true, 4, 16) == 0);
    const char *H = "X-NexusSync: 1\r\n";
    resp r;
    for (int i = 0; i < NX_LOGIN_FAILS_PER_USER; i++) {
        REQUIRE(req(f.port, "POST", "/api/auth/login", H,
                    "{\"username\":\"boss\",\"password\":\"guess-guess\"}", &r) == 0);
        CHECK(r.status == 401);
    }
    /* Locked for a minute, even with the right password. */
    REQUIRE(req(f.port, "POST", "/api/auth/login", H,
                "{\"username\":\"boss\",\"password\":\"admin-password-1\"}", &r) == 0);
    CHECK(r.status == 429 && strstr(r.body, "too_many_attempts"));
    /* Per IP: 20 failures over many usernames block this address. */
    for (int i = 0; i < NX_LOGIN_FAILS_PER_IP; i++) {
        char body[128];
        snprintf(body, sizeof body, "{\"username\":\"nobody%d\",\"password\":\"guess-guess\"}", i);
        REQUIRE(req(f.port, "POST", "/api/auth/login", H, body, &r) == 0);
    }
    REQUIRE(req(f.port, "POST", "/api/auth/login", H,
                "{\"username\":\"fresh-user\",\"password\":\"guess-guess\"}", &r) == 0);
    CHECK(r.status == 429);
    /* Wrong join codes count against the IP too. */
    REQUIRE(req(f.port, "POST", "/api/auth/join", H,
                "{\"username\":\"zed\",\"displayName\":\"Z\",\"password\":\"long-enough-1\",\"joinCode\":\"ABCD2345\"}",
                &r) == 0);
    CHECK(r.status == 429);
    fx_stop(&f);
}

/* ---- WebSocket ---------------------------------------------------------------------- */

static plat_sock ws_open(fx *f, const char *cookie_hdr, const char *extra, resp *r)
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
static int ws_read(plat_sock s, char *payload, size_t cap, size_t *len)
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

static int ws_send(plat_sock s, uint8_t opcode, const char *data, size_t n)
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

/* Reads frames until a text frame containing needle (skips pings). */
static bool ws_wait_for(plat_sock s, const char *needle)
{
    char buf[4096];
    size_t len;
    for (int i = 0; i < 20; i++) {
        int op = ws_read(s, buf, sizeof buf, &len);
        if (op < 0)
            return false;
        if (op == NX_WS_TEXT && strstr(buf, needle))
            return true;
    }
    return false;
}

static void test_websocket(void)
{
    fx f;
    REQUIRE(fx_start(&f, "srv_ws", true, 2, 16) == 0);
    resp r;
    plat_sock s = ws_open(&f, NULL, NULL, &r); /* no session */
    CHECK(r.status == 401);
    if (s != PLAT_SOCK_INVALID)
        plat_sock_close(s);
    s = ws_open(&f, f.admin_cookie, "Origin: http://evil.example\r\n", &r);
    CHECK(r.status == 403);
    if (s != PLAT_SOCK_INVALID)
        plat_sock_close(s);
    plat_sock bad = dial(f.port);
    const char *v8 = "GET /ws HTTP/1.1\r\nHost: h\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Version: 8\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    REQUIRE(plat_send_all(bad, v8, strlen(v8)) == 0 && read_resp(bad, &r) == 0);
    CHECK(r.status == 426 && strstr(r.head, "Sec-WebSocket-Version: 13"));
    plat_sock_close(bad);

    s = ws_open(&f, f.admin_cookie, "Origin: http://127.0.0.1\r\n", &r);
    REQUIRE(s != PLAT_SOCK_INVALID);
    CHECK(r.status == 101 && strstr(r.head, "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
    for (int i = 0; i < 100 && nx_hub_client_count(f.app->hub) == 0; i++)
        plat_sleep_ms(10);
    CHECK(nx_hub_client_count(f.app->hub) == 1);

    /* An event: an admin checkpoint after a change (login updated the
     * user record, so there are dirty pages). */
    char hdr[600];
    snprintf(hdr, sizeof hdr, "%sX-NexusSync: 1\r\n", f.admin_cookie);
    REQUIRE(req(f.port, "POST", "/api/admin/checkpoint", hdr, NULL, &r) == 0);
    CHECK(r.status == 200 && strstr(r.body, "\"pages\":"));
    CHECK(ws_wait_for(s, "\"type\":\"engine.checkpoint\""));

    /* Ping -> pong with the same payload. */
    REQUIRE(ws_send(s, NX_WS_PING, "hey", 3) == 0);
    char buf[256];
    size_t len = 0;
    int op;
    do {
        op = ws_read(s, buf, sizeof buf, &len);
    } while (op == NX_WS_TEXT || op == NX_WS_PING);
    CHECK(op == NX_WS_PONG && len == 3 && memcmp(buf, "hey", 3) == 0);

    /* Unmasked client frame: protocol error, the server closes with 1002. */
    uint8_t unmasked[] = {0x89, 0x00};
    REQUIRE(plat_send_all(s, unmasked, 2) == 0);
    do {
        op = ws_read(s, buf, sizeof buf, &len);
    } while (op == NX_WS_TEXT || op == NX_WS_PING);
    CHECK(op == NX_WS_CLOSE && len == 2 && (uint8_t)buf[0] == 0x03 && (uint8_t)buf[1] == 0xEA);
    plat_sock_close(s);
    for (int i = 0; i < 200 && nx_hub_client_count(f.app->hub) != 0; i++)
        plat_sleep_ms(10);
    CHECK(nx_hub_client_count(f.app->hub) == 0); /* reaped */

    /* A clean close, then server.shutdown to a client still connected. */
    s = ws_open(&f, f.admin_cookie, NULL, &r);
    REQUIRE(s != PLAT_SOCK_INVALID && r.status == 101);
    REQUIRE(ws_send(s, NX_WS_CLOSE, "\x03\xe8", 2) == 0);
    do {
        op = ws_read(s, buf, sizeof buf, &len);
    } while (op == NX_WS_TEXT || op == NX_WS_PING);
    CHECK(op == NX_WS_CLOSE);
    plat_sock_close(s);
    s = ws_open(&f, f.admin_cookie, NULL, &r);
    REQUIRE(s != PLAT_SOCK_INVALID && r.status == 101);
    for (int i = 0; i < 100 && nx_hub_client_count(f.app->hub) == 0; i++)
        plat_sleep_ms(10);
    atomic_store(&f.app->state, NX_APP_STOPPING);
    nx_server_stop(f.srv);
    nx_app_destroy(f.app);
    CHECK(ws_wait_for(s, "\"type\":\"server.shutdown\""));
    plat_sock_close(s);
}

static void test_busy_server_answers_503(void)
{
    fx f;
    REQUIRE(fx_start(&f, "srv_busy", true, 1, 2) == 0);
    /* Connection A keeps the only worker (idle keep-alive); B waits in the
     * queue; C exceeds max_connections = 2. */
    plat_sock a = dial(f.port), b, c;
    const char *one = "GET /api/health HTTP/1.1\r\nHost: h\r\n\r\n";
    resp r;
    REQUIRE(plat_send_all(a, one, strlen(one)) == 0 && read_resp(a, &r) == 0);
    b = dial(f.port);
    plat_sleep_ms(50);
    c = dial(f.port);
    REQUIRE(read_resp(c, &r) == 0);
    CHECK(r.status == 503 && strstr(r.body, "busy"));
    plat_sock_close(c);
    plat_sock_close(a); /* frees the worker: B is served */
    REQUIRE(plat_send_all(b, one, strlen(one)) == 0 && read_resp(b, &r) == 0);
    CHECK(r.status == 200);
    plat_sock_close(b);
    fx_stop(&f);
}

static const nx_test_case TESTS[] = {
    NX_CASE(test_recovering_then_ready),
    NX_CASE(test_static_files),
    NX_CASE(test_malformed_over_the_wire),
    NX_CASE(test_keep_alive_and_pipelining),
    NX_CASE(test_join_login_logout),
    NX_CASE(test_rate_limits),
    NX_CASE(test_websocket),
    NX_CASE(test_busy_server_answers_503),
};

int main(int argc, char **argv)
{
    if (plat_net_init() != 0)
        return 2;
    int rc = nx_test_run(TESTS, sizeof TESTS / sizeof TESTS[0], argc, argv);
    plat_net_cleanup();
    return rc;
}
