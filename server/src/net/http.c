/* http.c — see http.h. */
#include "net/http.h"

#include "util/json_out.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* RFC 9110 token characters (header names, methods). */
static bool is_tchar(unsigned char c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
        return true;
    return c && strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

/* Finds the end of the head: "\r\n\r\n" (or bare "\n\n", tolerated). */
static size_t find_head_end(const char *buf, size_t len)
{
    for (size_t i = 0; i + 1 < len; i++) {
        if (buf[i] == '\n' && buf[i + 1] == '\n')
            return i + 2;
        if (i + 3 < len && buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' &&
            buf[i + 3] == '\n')
            return i + 4;
    }
    return 0;
}

/* Cuts the next line (ending in \n or \r\n) out of [*p, end); NUL-
 * terminates it and advances *p. */
static char *next_line(char **p, char *end)
{
    char *s = *p, *nl = memchr(s, '\n', (size_t)(end - s));
    if (!nl)
        return NULL;
    *p = nl + 1;
    if (nl > s && nl[-1] == '\r')
        nl[-1] = '\0';
    *nl = '\0';
    return s;
}

static bool has_ctl(const char *s)
{
    for (; *s; s++)
        if (((unsigned char)*s < 0x20 && *s != '\t') || *s == 0x7f)
            return true;
    return false;
}

static int lower_eq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a;
        char y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (x != y)
            return 0;
    }
    return *a == *b;
}

/* True when a comma-separated header value contains token (any case). */
static bool list_has(const char *v, const char *token)
{
    size_t tl = strlen(token);
    while (*v) {
        while (*v == ' ' || *v == '\t' || *v == ',')
            v++;
        const char *s = v;
        while (*v && *v != ',')
            v++;
        const char *e = v;
        while (e > s && (e[-1] == ' ' || e[-1] == '\t'))
            e--;
        if ((size_t)(e - s) == tl) {
            char tmp[64];
            if (tl < sizeof tmp) {
                memcpy(tmp, s, tl);
                tmp[tl] = '\0';
                if (lower_eq(tmp, token))
                    return true;
            }
        }
    }
    return false;
}

int nx_http_parse_head(char *buf, size_t len, nx_http_req *req)
{
    memset(req, 0, sizeof *req);
    size_t scan = len < NX_HTTP_MAX_HEAD ? len : NX_HTTP_MAX_HEAD;
    size_t head_len = find_head_end(buf, scan);
    if (head_len == 0)
        return len >= NX_HTTP_MAX_HEAD ? 431 : NX_HTTP_NEED_MORE;
    if (memchr(buf, '\0', head_len))
        return 400;
    req->head_len = head_len;
    char *p = buf, *end = buf + head_len;

    /* Tolerate empty lines before the request line (RFC 9112 §2.2). */
    char *line;
    do {
        line = next_line(&p, end);
    } while (line && *line == '\0' && p < end);
    if (!line || !*line)
        return 400;

    /* Request line: METHOD SP target SP HTTP/1.x */
    char *sp1 = strchr(line, ' ');
    if (!sp1)
        return 400;
    char *sp2 = strchr(sp1 + 1, ' ');
    if (!sp2 || strchr(sp2 + 1, ' '))
        return 400;
    *sp1 = *sp2 = '\0';
    char *method = line, *target = sp1 + 1, *version = sp2 + 1;
    size_t ml = strlen(method);
    if (ml == 0)
        return 400;
    for (size_t i = 0; i < ml; i++)
        if (!is_tchar((unsigned char)method[i]))
            return 400;
    static const char *const METHODS[] = {"GET", "HEAD", "POST", "PUT", "PATCH", "DELETE", "OPTIONS"};
    bool known = false;
    for (size_t i = 0; i < sizeof METHODS / sizeof METHODS[0]; i++)
        if (strcmp(method, METHODS[i]) == 0)
            known = true;
    if (!known)
        return 501;
    memcpy(req->method, method, ml + 1);
    if (strlen(target) > NX_HTTP_MAX_TARGET)
        return 414;
    if (target[0] != '/' || has_ctl(target) || strchr(target, ' ') || strchr(target, '#'))
        return 400;
    if (strncmp(version, "HTTP/", 5) != 0)
        return 400;
    if (strcmp(version, "HTTP/1.1") == 0)
        req->minor = 1;
    else if (strcmp(version, "HTTP/1.0") == 0)
        req->minor = 0;
    else
        return 505;
    char *q = strchr(target, '?');
    if (q) {
        *q = '\0';
        req->query = q + 1;
    } else {
        req->query = "";
    }
    req->path = target;

    /* Headers. */
    const char *connection = NULL;
    while (p < end) {
        line = next_line(&p, end);
        if (!line)
            return 400;
        if (*line == '\0')
            break; /* end of head */
        if (*line == ' ' || *line == '\t')
            return 400; /* obsolete line folding */
        char *colon = strchr(line, ':');
        if (!colon || colon == line)
            return 400;
        for (char *c = line; c < colon; c++) {
            if (!is_tchar((unsigned char)*c))
                return 400; /* includes whitespace before the colon */
            if (*c >= 'A' && *c <= 'Z')
                *c = (char)(*c + 32);
        }
        *colon = '\0';
        char *v = colon + 1;
        while (*v == ' ' || *v == '\t')
            v++;
        size_t vl = strlen(v);
        while (vl && (v[vl - 1] == ' ' || v[vl - 1] == '\t'))
            v[--vl] = '\0';
        if (has_ctl(v))
            return 400;
        if (req->nheaders == NX_HTTP_MAX_HEADERS)
            return 431;
        req->headers[req->nheaders++] = (nx_http_header){line, v};

        if (strcmp(line, "content-length") == 0) {
            if (!*v || strlen(v) > 15)
                return 400;
            uint64_t n = 0;
            for (const char *d = v; *d; d++) {
                if (*d < '0' || *d > '9')
                    return 400;
                n = n * 10 + (uint64_t)(*d - '0');
            }
            if (req->has_body_length && n != req->content_length)
                return 400; /* conflicting lengths: a request smuggling trick */
            req->has_body_length = true;
            req->content_length = n;
        } else if (strcmp(line, "transfer-encoding") == 0) {
            return 411;
        } else if (strcmp(line, "connection") == 0) {
            connection = v;
        } else if (strcmp(line, "expect") == 0) {
            if (!lower_eq(v, "100-continue"))
                return 417;
            req->expect_continue = true;
        }
    }
    if (req->minor == 1 && !nx_http_get_header(req, "host"))
        return 400;
    if (req->minor == 1)
        req->keep_alive = !(connection && list_has(connection, "close"));
    else
        req->keep_alive = connection && list_has(connection, "keep-alive");
    return NX_HTTP_OK;
}

const char *nx_http_get_header(const nx_http_req *req, const char *lower_name)
{
    for (size_t i = 0; i < req->nheaders; i++)
        if (strcmp(req->headers[i].name, lower_name) == 0)
            return req->headers[i].value;
    return NULL;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

int nx_url_decode(const char *in, size_t n, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        char c = in[i];
        if (c == '%') {
            if (i + 2 >= n) /* both hex digits must be inside the input */
                return -1;
            int h = hexval(in[i + 1]), l = hexval(in[i + 2]);
            if (h < 0 || l < 0)
                return -1;
            c = (char)(h * 16 + l);
            i += 2;
        }
        if (c == '\0' || o + 1 >= cap)
            return -1;
        out[o++] = c;
    }
    out[o] = '\0';
    return 0;
}

bool nx_query_get(const char *query, const char *key, char *out, size_t cap)
{
    size_t kl = strlen(key);
    const char *p = query;
    while (p && *p) {
        const char *amp = strchr(p, '&');
        size_t n = amp ? (size_t)(amp - p) : strlen(p);
        if (n > kl && strncmp(p, key, kl) == 0 && p[kl] == '=')
            return nx_url_decode(p + kl + 1, n - kl - 1, out, cap) == 0;
        if (n == kl && strncmp(p, key, kl) == 0) { /* "key" with no '=' */
            if (cap == 0)
                return false;
            out[0] = '\0';
            return true;
        }
        p = amp ? amp + 1 : NULL;
    }
    return false;
}

bool nx_cookie_get(const char *h, const char *name, char *out, size_t cap)
{
    size_t nl = strlen(name);
    const char *p = h;
    while (p && *p) {
        while (*p == ' ' || *p == ';')
            p++;
        const char *semi = strchr(p, ';');
        size_t n = semi ? (size_t)(semi - p) : strlen(p);
        if (n > nl && strncmp(p, name, nl) == 0 && p[nl] == '=') {
            size_t vl = n - nl - 1;
            if (vl + 1 > cap)
                return false;
            memcpy(out, p + nl + 1, vl);
            out[vl] = '\0';
            return true;
        }
        p = semi;
    }
    return false;
}

const char *nx_http_reason(int status)
{
    switch (status) {
    case 100: return "Continue";
    case 101: return "Switching Protocols";
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 415: return "Unsupported Media Type";
    case 417: return "Expectation Failed";
    case 422: return "Unprocessable Content";
    case 426: return "Upgrade Required";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    case 505: return "HTTP Version Not Supported";
    case 507: return "Insufficient Storage";
    default: return "Unknown";
    }
}

/* Sent on every response (SPEC §11). 'wasm-unsafe-eval' is needed by
 * hash-wasm; img data: URLs are used by the connect-screen QR codes. */
#define SECURITY_HEADERS                                                                      \
    "X-Content-Type-Options: nosniff\r\n"                                                     \
    "Referrer-Policy: no-referrer\r\n"                                                        \
    "X-Frame-Options: DENY\r\n"                                                               \
    "Content-Security-Policy: default-src 'self'; script-src 'self' 'wasm-unsafe-eval'; "     \
    "style-src 'self'; img-src 'self' data:; font-src 'self'; connect-src 'self'; "           \
    "object-src 'none'; base-uri 'none'; form-action 'self'; frame-ancestors 'none'\r\n"

size_t nx_http_format_head(char *buf, size_t cap, int status, const char *content_type,
                           uint64_t content_length, const char *extra, bool keep_alive)
{
    int n = snprintf(buf, cap,
                     "HTTP/1.1 %d %s\r\n" SECURITY_HEADERS "%s%s%s"
                     "Content-Length: %" PRIu64 "\r\n"
                     "Connection: %s\r\n"
                     "%s\r\n",
                     status, nx_http_reason(status), content_type ? "Content-Type: " : "",
                     content_type ? content_type : "", content_type ? "\r\n" : "", content_length,
                     keep_alive ? "keep-alive" : "close", extra ? extra : "");
    return n < 0 || (size_t)n >= cap ? 0 : (size_t)n;
}

int nx_http_send(plat_sock s, int status, const char *content_type, const char *extra,
                 const void *body, size_t len, bool keep_alive, bool head_only)
{
    char head[2048];
    size_t hl = nx_http_format_head(head, sizeof head, status, content_type,
                                    status == 204 || status == 304 ? 0 : len, extra, keep_alive);
    if (hl == 0)
        return -1;
    if (plat_send_all(s, head, hl) != 0)
        return -1;
    if (!head_only && len && status != 204 && status != 304)
        return plat_send_all(s, body, len);
    return 0;
}

int nx_http_send_error(plat_sock s, int status, const char *code, const char *message,
                       bool keep_alive)
{
    nx_json j;
    nx_json_init(&j);
    nx_json_obj_open(&j);
    nx_json_key(&j, "error");
    nx_json_obj_open(&j);
    nx_json_key_str(&j, "code", code);
    nx_json_key_str(&j, "message", message);
    nx_json_obj_close(&j);
    nx_json_obj_close(&j);
    int rc = j.oom ? -1
                   : nx_http_send(s, status, "application/json; charset=utf-8", NULL, j.buf, j.len,
                                  keep_alive, false);
    nx_json_free(&j);
    return rc;
}
