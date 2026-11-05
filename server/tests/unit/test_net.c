/*
 * test_net.c — the network layer's pure pieces (SPEC §11–§13):
 *   - HTTP request heads: valid requests and every malformed or oversized
 *     shape we refuse (400, 411, 414, 431, 501, 505), at every split point
 *   - percent-decoding, query strings, cookies, response heads
 *   - WebSocket accept key (RFC 6455 example) and frame parsing rules
 *   - JSON output escaping, config parsing, static path safety
 *   - usernames, display names, UTF-8, passwords
 *   - sessions (create, look up, expire, delete) and login rate limits
 */
#include "api/auth.h"
#include "net/http.h"
#include "net/static.h"
#include "net/ws.h"
#include "nx_test.h"
#include "nx_test_util.h"
#include "util/config.h"
#include "util/json_out.h"

static int parse(const char *text, nx_http_req *r, char *buf)
{
    size_t n = strlen(text);
    memcpy(buf, text, n);
    return nx_http_parse_head(buf, n, r);
}

static void test_http_valid(void)
{
    static char buf[40000];
    nx_http_req r;
    REQUIRE(parse("GET /api/files?q=lab%203&sort=name HTTP/1.1\r\nHost: x\r\n"
                  "Cookie: a=1; nx_session=abc\r\nX-Thing:   padded value  \r\n\r\nBODY",
                  &r, buf) == NX_HTTP_OK);
    CHECK_STR(r.method, "GET");
    CHECK_STR(r.path, "/api/files");
    CHECK_STR(r.query, "q=lab%203&sort=name");
    CHECK(r.minor == 1 && r.keep_alive && !r.has_body_length);
    CHECK_STR(nx_http_get_header(&r, "x-thing"), "padded value");
    CHECK_STR(nx_http_get_header(&r, "host"), "x");
    CHECK(nx_http_get_header(&r, "missing") == NULL);
    CHECK_STR(buf + r.head_len, "BODY");
    char v[64];
    CHECK(nx_query_get(r.query, "q", v, sizeof v) && strcmp(v, "lab 3") == 0);
    CHECK(nx_query_get(r.query, "sort", v, sizeof v) && strcmp(v, "name") == 0);
    CHECK(!nx_query_get(r.query, "so", v, sizeof v));
    CHECK(nx_cookie_get(nx_http_get_header(&r, "cookie"), "nx_session", v, sizeof v) &&
          strcmp(v, "abc") == 0);
    CHECK(!nx_cookie_get("nx_sessionx=1", "nx_session", v, sizeof v));

    REQUIRE(parse("POST /x HTTP/1.1\r\nHost: h\r\nContent-Length: 12\r\nConnection: close\r\n"
                  "Expect: 100-continue\r\n\r\n",
                  &r, buf) == NX_HTTP_OK);
    CHECK(r.has_body_length && r.content_length == 12 && !r.keep_alive && r.expect_continue);
    /* HTTP/1.0: no keep-alive unless asked; Host not required. */
    REQUIRE(parse("GET / HTTP/1.0\r\n\r\n", &r, buf) == NX_HTTP_OK);
    CHECK(r.minor == 0 && !r.keep_alive);
    REQUIRE(parse("GET / HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n", &r, buf) == NX_HTTP_OK);
    CHECK(r.keep_alive);
    /* Bare LF line endings are tolerated; repeated equal lengths are fine. */
    REQUIRE(parse("PUT /y HTTP/1.1\nHost: h\nContent-Length: 3\ncontent-length: 3\n\n", &r, buf) ==
            NX_HTTP_OK);
    CHECK(r.content_length == 3);
    /* Every split point of a valid head asks for more until complete. */
    const char *full = "GET /a HTTP/1.1\r\nHost: h\r\n\r\n";
    for (size_t cut = 0; cut < strlen(full); cut++) {
        memcpy(buf, full, cut);
        CHECK(nx_http_parse_head(buf, cut, &r) == NX_HTTP_NEED_MORE);
    }
}

static void test_http_malformed(void)
{
    static char buf[40000];
    nx_http_req r;
    struct {
        const char *text;
        int want;
    } cases[] = {
        {"GARBAGE\r\n\r\n", 400},
        {"GET /\r\n\r\n", 400},                              /* no version */
        {"GET  / HTTP/1.1\r\nHost: h\r\n\r\n", 400},         /* double space */
        {"GET / HTTP/1.1 extra\r\nHost: h\r\n\r\n", 400},
        {"GET http://evil/ HTTP/1.1\r\nHost: h\r\n\r\n", 400}, /* absolute form */
        {"GET / HTTP/2.0\r\nHost: h\r\n\r\n", 505},
        {"GET / FTP/1.1\r\nHost: h\r\n\r\n", 400},
        {"BREW / HTTP/1.1\r\nHost: h\r\n\r\n", 501},
        {"GET / HTTP/1.1\r\n\r\n", 400},                     /* no Host */
        {"GET / HTTP/1.1\r\nHost: h\r\nNoColon\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nHost: h\r\nBad Name: v\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nHost : h\r\n\r\n", 400},        /* space before colon */
        {"GET / HTTP/1.1\r\nHost: h\r\nX: a\r\n folded\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nHost: h\r\nX: a\x01b\r\n\r\n", 400},
        {"POST / HTTP/1.1\r\nHost: h\r\nContent-Length: -1\r\n\r\n", 400},
        {"POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 1x\r\n\r\n", 400},
        {"POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 99999999999999999\r\n\r\n", 400},
        {"POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 3\r\nContent-Length: 4\r\n\r\n", 400},
        {"POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n", 411},
        {"POST / HTTP/1.1\r\nHost: h\r\nExpect: something\r\n\r\n", 417},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int got = parse(cases[i].text, &r, buf);
        if (got != cases[i].want)
            fprintf(stderr, "    case %zu: got %d want %d\n", i, got, cases[i].want);
        CHECK(got == cases[i].want);
    }
    /* NUL inside the head. */
    memcpy(buf, "GET /a\0b HTTP/1.1\r\nHost: h\r\n\r\n", 31);
    CHECK(nx_http_parse_head(buf, 31, &r) == 400);
    /* Oversized: 16 KB without the end of the head, or a target over 8 KB,
     * or more than 64 headers. */
    memset(buf, 'a', 20000);
    memcpy(buf, "GET /", 5);
    CHECK(nx_http_parse_head(buf, 20000, &r) == 431);
    CHECK(nx_http_parse_head(buf, 16000, &r) == NX_HTTP_NEED_MORE);
    int n = snprintf(buf, sizeof buf, "GET /");
    memset(buf + n, 'p', 9000);
    n += 9000;
    n += snprintf(buf + n, sizeof buf - (size_t)n, " HTTP/1.1\r\nHost: h\r\n\r\n");
    CHECK(nx_http_parse_head(buf, (size_t)n, &r) == 414);
    n = snprintf(buf, sizeof buf, "GET / HTTP/1.1\r\nHost: h\r\n");
    for (int i = 0; i < 70; i++)
        n += snprintf(buf + n, sizeof buf - (size_t)n, "X-%d: v\r\n", i);
    n += snprintf(buf + n, sizeof buf - (size_t)n, "\r\n");
    CHECK(nx_http_parse_head(buf, (size_t)n, &r) == 431);
    /* Random bytes never crash the parser (run under ASan). */
    nx_splitmix rng = {42};
    for (int t = 0; t < 2000; t++) {
        size_t len = (size_t)(nx_splitmix_next(&rng) % 600);
        for (size_t k = 0; k < len; k++) {
            uint64_t z = nx_splitmix_next(&rng);
            const char alphabet[] = "GET /?&=%: \r\n\tHTP1.0aZ\x00\x7f\xff";
            buf[k] = alphabet[z % (sizeof alphabet - 1)];
        }
        int rc = nx_http_parse_head(buf, len, &r);
        CHECK(rc == NX_HTTP_OK || rc == NX_HTTP_NEED_MORE || rc >= 400);
    }
}

static void test_url_decode_and_heads(void)
{
    char out[32];
    CHECK(nx_url_decode("/a%20b", 6, out, sizeof out) == 0 && strcmp(out, "/a b") == 0);
    CHECK(nx_url_decode("%2e%2E", 6, out, sizeof out) == 0 && strcmp(out, "..") == 0);
    CHECK(nx_url_decode("%00", 3, out, sizeof out) != 0);  /* NUL */
    CHECK(nx_url_decode("%4", 2, out, sizeof out) != 0);   /* truncated */
    CHECK(nx_url_decode("%zz", 3, out, sizeof out) != 0);
    CHECK(nx_url_decode("abcdef", 6, out, 4) != 0);        /* too long */
    char head[4096];
    size_t n = nx_http_format_head(head, sizeof head, 404, "text/plain", 5, "X-A: b\r\n", false);
    CHECK(n > 0 && strncmp(head, "HTTP/1.1 404 Not Found\r\n", 24) == 0);
    CHECK(strstr(head, "Content-Length: 5\r\n") && strstr(head, "Connection: close\r\n"));
    CHECK(strstr(head, "X-Content-Type-Options: nosniff\r\n") &&
          strstr(head, "X-Frame-Options: DENY\r\n") && strstr(head, "'wasm-unsafe-eval'"));
    CHECK(strstr(head, "X-A: b\r\n\r\n") != NULL);
    CHECK(nx_http_format_head(head, 50, 200, NULL, 0, NULL, true) == 0); /* does not fit */
}

static void test_ws_codec(void)
{
    char acc[29];
    /* RFC 6455 §1.3 example. */
    REQUIRE(nx_ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==", acc) == 0);
    CHECK_STR(acc, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    CHECK(nx_ws_accept_key("short", acc) != 0);
    CHECK(nx_ws_accept_key("dGhlIHNhbXBsZSBub25jZQ!=", acc) != 0);

    /* A masked text frame "Hello" (RFC 6455 §5.7). */
    uint8_t f1[] = {0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58};
    nx_ws_frame f;
    for (size_t cut = 0; cut < sizeof f1; cut++)
        CHECK(nx_ws_parse_frame(f1, cut, 4096, &f) == NX_WS_NEED_MORE);
    REQUIRE(nx_ws_parse_frame(f1, sizeof f1, 4096, &f) == 0);
    CHECK(f.fin && f.opcode == NX_WS_TEXT && f.payload_len == 5 && f.header_len == 6);
    nx_ws_unmask(f1 + 6, 5, f.mask);
    CHECK(memcmp(f1 + 6, "Hello", 5) == 0);

    uint8_t unmasked[] = {0x81, 0x05, 'H', 'e', 'l', 'l', 'o'};
    CHECK(nx_ws_parse_frame(unmasked, sizeof unmasked, 4096, &f) == NX_WS_PROTOCOL_ERROR);
    uint8_t rsv[] = {0xC1, 0x80, 0, 0, 0, 0};
    CHECK(nx_ws_parse_frame(rsv, sizeof rsv, 4096, &f) == NX_WS_PROTOCOL_ERROR);
    uint8_t bad_op[] = {0x83, 0x80, 0, 0, 0, 0};
    CHECK(nx_ws_parse_frame(bad_op, sizeof bad_op, 4096, &f) == NX_WS_PROTOCOL_ERROR);
    uint8_t frag_ping[] = {0x09, 0x80, 0, 0, 0, 0};
    CHECK(nx_ws_parse_frame(frag_ping, sizeof frag_ping, 4096, &f) == NX_WS_PROTOCOL_ERROR);
    uint8_t big_ping[] = {0x89, 0xFE, 0x00, 0x7E};
    CHECK(nx_ws_parse_frame(big_ping, sizeof big_ping, 4096, &f) == NX_WS_PROTOCOL_ERROR);
    uint8_t huge[] = {0x82, 0xFF, 0, 0, 0, 0, 0, 1, 0, 0};
    CHECK(nx_ws_parse_frame(huge, sizeof huge, 4096, &f) == NX_WS_TOO_BIG);

    uint8_t h[10];
    CHECK(nx_ws_frame_header(h, NX_WS_TEXT, 5) == 2 && h[0] == 0x81 && h[1] == 5);
    CHECK(nx_ws_frame_header(h, NX_WS_TEXT, 300) == 4 && h[1] == 126 && h[2] == 1 && h[3] == 44);
    CHECK(nx_ws_frame_header(h, NX_WS_BINARY, 70000) == 10 && h[1] == 127 && h[7] == 1 &&
          h[8] == 0x11 && h[9] == 0x70);
}

static void test_json_out(void)
{
    nx_json j;
    nx_json_init(&j);
    nx_json_obj_open(&j);
    nx_json_key_str(&j, "s", "a\"b\\c\n<x>&\x01");
    nx_json_key_u64(&j, "n", 18446744073709551615ull);
    nx_json_key(&j, "arr");
    nx_json_arr_open(&j);
    nx_json_i64(&j, -5);
    nx_json_bool(&j, true);
    nx_json_null(&j);
    nx_json_f64(&j, 0.5);
    nx_json_obj_open(&j);
    nx_json_obj_close(&j);
    nx_json_arr_close(&j);
    nx_json_key_str(&j, "utf8", "Ré");
    nx_json_obj_close(&j);
    REQUIRE(!j.oom);
    CHECK_STR(j.buf, "{\"s\":\"a\\\"b\\\\c\\n\\u003cx\\u003e\\u0026\\u0001\",\"n\":18446744073709551615,"
                     "\"arr\":[-5,true,null,0.5,{}],\"utf8\":\"Ré\"}");
    nx_json_free(&j);
    char b64[64];
    nx_base64_encode((const uint8_t *)"foobar", 6, b64);
    CHECK_STR(b64, "Zm9vYmFy");
    nx_base64_encode((const uint8_t *)"fooba", 5, b64);
    CHECK_STR(b64, "Zm9vYmE=");
    nx_base64_encode((const uint8_t *)"f", 1, b64);
    CHECK_STR(b64, "Zg==");
}

static void test_config(void)
{
    nx_config c;
    nx_config_defaults(&c);
    char err[200];
    REQUIRE(nx_config_parse("# comment\n\nlisten = 127.0.0.1:9090  # trailing\r\nworkers=4\n"
                            "chunker = fixed\nverify_on_read = false\njoin_code = abcd2345\n",
                            &c, err, sizeof err) == 0);
    CHECK_STR(c.listen_host, "127.0.0.1");
    CHECK(c.listen_port == 9090 && c.workers == 4 && c.chunker == 0 && !c.verify_on_read);
    CHECK(c.max_connections == 64 && c.session_ttl_s == 43200); /* untouched defaults */
    CHECK(nx_config_parse("worker = 4\n", &c, err, sizeof err) != 0); /* typo: unknown key */
    CHECK(strstr(err, "line 1") && strstr(err, "unknown key"));
    CHECK(nx_config_parse("x\ny\n", &c, err, sizeof err) != 0);
    CHECK(nx_config_parse("workers = 0\n", &c, err, sizeof err) != 0);
    CHECK(nx_config_parse("workers = -3\n", &c, err, sizeof err) != 0);
    CHECK(nx_config_parse("workers = 12abc\n", &c, err, sizeof err) != 0);
    CHECK(nx_config_parse("listen = 0.0.0.0:70000\n", &c, err, sizeof err) != 0);
    CHECK(nx_config_parse("join_code = ABCD1234\n", &c, err, sizeof err) != 0); /* '1' not allowed */
    CHECK(nx_config_parse("listen = :8081\n", &c, err, sizeof err) == 0 && c.listen_port == 8081 &&
          strcmp(c.listen_host, "0.0.0.0") == 0);
}

static void test_static_paths(void)
{
    CHECK(nx_static_path_ok("/") == 0);
    CHECK(nx_static_path_ok("/assets/index-abc.js") == 0);
    CHECK(nx_static_path_ok("/files/12") == 0);
    CHECK(nx_static_path_ok("/../etc/passwd") != 0);
    CHECK(nx_static_path_ok("/assets/../../x") != 0);
    CHECK(nx_static_path_ok("/./x") != 0);
    CHECK(nx_static_path_ok("/.env") != 0);
    CHECK(nx_static_path_ok("//etc/passwd") != 0);
    CHECK(nx_static_path_ok("/a\\b") != 0);
    CHECK(nx_static_path_ok("/C:/Windows") != 0);
    CHECK(nx_static_path_ok("x") != 0);
    CHECK_STR(nx_static_mime("/assets/a.js"), "text/javascript; charset=utf-8");
    CHECK_STR(nx_static_mime("/fonts/Geist.woff2"), "font/woff2");
    CHECK_STR(nx_static_mime("/a.dir/noext"), "application/octet-stream");
}

static void test_validation(void)
{
    char u[NX_USERNAME_MAX + 1];
    CHECK(nx_username_normalize("Riya.K-2", u) && strcmp(u, "riya.k-2") == 0);
    CHECK(!nx_username_normalize("ab", u));
    CHECK(!nx_username_normalize("-dash", u));
    CHECK(!nx_username_normalize("has space", u));
    CHECK(!nx_username_normalize("über", u));
    CHECK(!nx_username_normalize("abcdefghijklmnopqrstuvwxyz0123456", u)); /* 33 */
    CHECK(nx_display_valid("Riya Kapoor"));
    CHECK(nx_display_valid("राहुल"));
    CHECK(!nx_display_valid(""));
    CHECK(!nx_display_valid(" padded"));
    CHECK(!nx_display_valid("tab\there"));
    CHECK(!nx_utf8_valid("\xC0\xAF", 2));         /* overlong '/' */
    CHECK(!nx_utf8_valid("\xED\xA0\x80", 3));     /* surrogate */
    CHECK(!nx_utf8_valid("\xF4\x90\x80\x80", 4)); /* > U+10FFFF */
    CHECK(!nx_utf8_valid("\xE2\x82", 2));         /* truncated */
    CHECK(nx_utf8_valid("\xE2\x82\xAC", 3));      /* € */

    nx_user usr;
    memset(&usr, 0, sizeof usr);
    REQUIRE(nx_password_set(&usr, "correct horse", 1000) == 0);
    CHECK(usr.pbkdf2_iters == 1000);
    CHECK(nx_password_check(&usr, "correct horse"));
    CHECK(!nx_password_check(&usr, "correct horsE"));
    nx_user other = usr;
    REQUIRE(nx_password_set(&other, "correct horse", 1000) == 0);
    CHECK(memcmp(usr.salt, other.salt, 16) != 0 && memcmp(usr.pw_hash, other.pw_hash, 32) != 0);
}

static void test_sessions_and_rate_limits(void)
{
    nx_auth *a;
    REQUIRE(nx_auth_create(3600, &a) == 0);
    char t1[65], t2[65], t3[65];
    REQUIRE(nx_session_create(a, 7, t1) == 0 && nx_session_create(a, 7, t2) == 0 &&
            nx_session_create(a, 9, t3) == 0);
    CHECK(strlen(t1) == 64 && strcmp(t1, t2) != 0);
    uint32_t uid = 0;
    CHECK(nx_session_lookup(a, t1, &uid) && uid == 7);
    CHECK(nx_session_lookup(a, t3, &uid) && uid == 9);
    char forged[65];
    memcpy(forged, t1, 65);
    forged[0] = forged[0] == 'a' ? 'b' : 'a';
    CHECK(!nx_session_lookup(a, forged, &uid));
    CHECK(!nx_session_lookup(a, "nothex", &uid));
    nx_session_delete(a, t1);
    CHECK(!nx_session_lookup(a, t1, &uid) && nx_session_lookup(a, t2, &uid));
    nx_session_delete_user(a, 7);
    CHECK(!nx_session_lookup(a, t2, &uid) && nx_session_lookup(a, t3, &uid));
    CHECK_EQ_U64(nx_session_count(a), 1);
    for (int i = 0; i < 300; i++) /* grows the table */
        REQUIRE(nx_session_create(a, (uint32_t)i, t1) == 0);
    CHECK(nx_session_lookup(a, t1, &uid) && uid == 299);
    nx_auth_destroy(a);

    REQUIRE(nx_auth_create(0, &a) == 0); /* ttl 0: expired at once */
    REQUIRE(nx_session_create(a, 1, t1) == 0);
    CHECK(!nx_session_lookup(a, t1, &uid));
    nx_auth_destroy(a);

    REQUIRE(nx_auth_create(3600, &a) == 0);
    uint64_t now = 1000000;
    for (int i = 0; i < NX_LOGIN_FAILS_PER_USER; i++) {
        CHECK(!nx_rl_blocked(a, "riya", "10.0.0.5", now));
        nx_rl_fail(a, "riya", "10.0.0.5", now + (uint64_t)i);
    }
    CHECK(nx_rl_blocked(a, "riya", "10.0.0.9", now + 10)); /* per username, any IP */
    CHECK(!nx_rl_blocked(a, "kabir", "10.0.0.9", now + 10));
    CHECK(!nx_rl_blocked(a, "riya", "10.0.0.9", now + 60010)); /* the window slides */
    /* 20 failures from one IP over many usernames block that IP. */
    for (int i = 0; i < NX_LOGIN_FAILS_PER_IP; i++) {
        char name[16];
        snprintf(name, sizeof name, "u%d", i);
        nx_rl_fail(a, name, "10.0.0.66", now + 100000);
    }
    CHECK(nx_rl_blocked(a, "fresh", "10.0.0.66", now + 100001));
    CHECK(!nx_rl_blocked(a, "fresh", "10.0.0.67", now + 100001));
    CHECK(!nx_rl_blocked(a, "fresh", "10.0.0.66", now + 160001));
    /* More keys than slots: old ones are evicted, nothing breaks. */
    for (int i = 0; i < 3000; i++) {
        char ip[32];
        snprintf(ip, sizeof ip, "10.1.%d.%d", i / 250, i % 250);
        nx_rl_fail(a, NULL, ip, now + 200000 + (uint64_t)i);
    }
    nx_auth_destroy(a);
}

static const nx_test_case TESTS[] = {
    NX_CASE(test_http_valid),
    NX_CASE(test_http_malformed),
    NX_CASE(test_url_decode_and_heads),
    NX_CASE(test_ws_codec),
    NX_CASE(test_json_out),
    NX_CASE(test_config),
    NX_CASE(test_static_paths),
    NX_CASE(test_validation),
    NX_CASE(test_sessions_and_rate_limits),
};

NX_TEST_MAIN(TESTS)
