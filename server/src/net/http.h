/*
 * http.h — HTTP/1.1 request heads and responses (SPEC §11), from scratch.
 *
 * The parser works on a buffer that holds the bytes received so far. It is
 * strict, because every byte comes from the network and is hostile until
 * proven otherwise:
 *   - request line + headers at most 16 KB (else 431), at most 64 headers
 *   - origin-form targets only ("/path?query"), at most 8 KB (else 414)
 *   - HTTP/1.0 and HTTP/1.1 only (else 505); known methods only (else 501)
 *   - Host required on 1.1; no obsolete line folding; header names are
 *     tokens; no NUL or other control bytes anywhere
 *   - Content-Length must be plain digits, and repeated copies must agree;
 *     any Transfer-Encoding (chunked bodies) is refused with 411
 * It parses in place: header names and values are NUL-terminated inside
 * the caller's buffer, so the buffer must outlive the request.
 */
#ifndef NX_HTTP_H
#define NX_HTTP_H

#include "platform/platform.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NX_HTTP_MAX_HEAD 16384u
#define NX_HTTP_MAX_HEADERS 64u
#define NX_HTTP_MAX_TARGET 8192u

#define NX_HTTP_OK 0
#define NX_HTTP_NEED_MORE 1
/* Any other return value is the HTTP status to answer with (400, 411,
 * 414, 431, 501, 505). */

typedef struct {
    const char *name;  /* lower-cased in place */
    const char *value; /* surrounding whitespace removed */
} nx_http_header;

typedef struct {
    char method[8];
    const char *path;  /* raw (still percent-encoded), without the query */
    const char *query; /* after '?', or "" */
    int minor;         /* 0 for HTTP/1.0, 1 for HTTP/1.1 */
    nx_http_header headers[NX_HTTP_MAX_HEADERS];
    size_t nheaders;
    bool has_body_length;
    uint64_t content_length;
    bool keep_alive;
    bool expect_continue;
    size_t head_len; /* bytes of buf used by the head; the body follows */
} nx_http_req;

int nx_http_parse_head(char *buf, size_t len, nx_http_req *req);
/* Case-insensitive lookup (names are stored lower-case). NULL if absent. */
const char *nx_http_get_header(const nx_http_req *req, const char *lower_name);

/* Percent-decodes in[0..n) into out (NUL-terminated). Refuses bad escapes,
 * NUL bytes, and output longer than cap - 1. '+' is kept as '+'. */
int nx_url_decode(const char *in, size_t n, char *out, size_t cap);
/* Finds key in an "a=1&b=2" query and decodes its value into out. */
bool nx_query_get(const char *query, const char *key, char *out, size_t cap);
/* Value of a cookie in a Cookie header ("a=1; nx_session=..."), copied into
 * out. False if absent or too long. */
bool nx_cookie_get(const char *cookie_header, const char *name, char *out, size_t cap);

const char *nx_http_reason(int status);

/* Writes a response head (status line, our security headers, Content-Type,
 * Content-Length, Connection, extra lines each ending in \r\n) into buf.
 * Returns its length, or 0 if it did not fit. */
size_t nx_http_format_head(char *buf, size_t cap, int status, const char *content_type,
                           uint64_t content_length, const char *extra, bool keep_alive);

/* Sends head + body (body skipped for HEAD requests when head_only). */
int nx_http_send(plat_sock s, int status, const char *content_type, const char *extra,
                 const void *body, size_t len, bool keep_alive, bool head_only);
/* Sends {"error":{"code":..,"message":..}} with the given status. */
int nx_http_send_error(plat_sock s, int status, const char *code, const char *message,
                       bool keep_alive);

#endif
