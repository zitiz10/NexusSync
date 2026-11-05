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
#include "nx_server_util.h"

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
