/* json_out.c — see json_out.h. */
#include "util/json_out.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void nx_json_init(nx_json *j) { memset(j, 0, sizeof *j); }

void nx_json_free(nx_json *j)
{
    free(j->buf);
    memset(j, 0, sizeof *j);
}

static void put(nx_json *j, const char *s, size_t n)
{
    if (j->oom)
        return;
    if (j->cap - j->len < n + 1) {
        size_t cap = j->cap ? j->cap : 256;
        while (cap - j->len < n + 1)
            cap *= 2;
        char *b = realloc(j->buf, cap);
        if (!b) {
            j->oom = true;
            return;
        }
        j->buf = b;
        j->cap = cap;
    }
    memcpy(j->buf + j->len, s, n);
    j->len += n;
    j->buf[j->len] = '\0';
}

/* Before a value or key: a comma if a sibling came before. */
static void sep(nx_json *j)
{
    if (j->need_comma)
        put(j, ",", 1);
    j->need_comma = true;
}

void nx_json_obj_open(nx_json *j)
{
    sep(j);
    put(j, "{", 1);
    j->need_comma = false;
}

void nx_json_obj_close(nx_json *j)
{
    put(j, "}", 1);
    j->need_comma = true;
}

void nx_json_arr_open(nx_json *j)
{
    sep(j);
    put(j, "[", 1);
    j->need_comma = false;
}

void nx_json_arr_close(nx_json *j)
{
    put(j, "]", 1);
    j->need_comma = true;
}

static void put_escaped(nx_json *j, const char *s, size_t n)
{
    put(j, "\"", 1);
    size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        const char *esc = NULL;
        char tmp[8];
        switch (ch) {
        case '"': esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        case '\n': esc = "\\n"; break;
        case '\r': esc = "\\r"; break;
        case '\t': esc = "\\t"; break;
        default:
            /* Control characters, and < > & so JSON can never be read as
             * markup if it ends up inside HTML. */
            if (ch < 0x20 || ch == 0x7f || ch == '<' || ch == '>' || ch == '&') {
                snprintf(tmp, sizeof tmp, "\\u%04x", ch);
                esc = tmp;
            }
        }
        if (esc) {
            put(j, s + start, i - start);
            put(j, esc, strlen(esc));
            start = i + 1;
        }
    }
    put(j, s + start, n - start);
    put(j, "\"", 1);
}

void nx_json_key(nx_json *j, const char *key)
{
    sep(j);
    put_escaped(j, key, strlen(key));
    put(j, ":", 1);
    j->need_comma = false;
}

void nx_json_str(nx_json *j, const char *s) { nx_json_strn(j, s, strlen(s)); }

void nx_json_strn(nx_json *j, const char *s, size_t n)
{
    sep(j);
    put_escaped(j, s, n);
}

void nx_json_u64(nx_json *j, uint64_t v)
{
    char b[24];
    int n = snprintf(b, sizeof b, "%" PRIu64, v);
    sep(j);
    put(j, b, (size_t)n);
}

void nx_json_i64(nx_json *j, int64_t v)
{
    char b[24];
    int n = snprintf(b, sizeof b, "%" PRId64, v);
    sep(j);
    put(j, b, (size_t)n);
}

void nx_json_f64(nx_json *j, double v)
{
    if (!isfinite(v)) {
        nx_json_null(j);
        return;
    }
    char b[40];
    int n = snprintf(b, sizeof b, "%.6g", v);
    sep(j);
    put(j, b, (size_t)n);
}

void nx_json_bool(nx_json *j, bool v)
{
    sep(j);
    put(j, v ? "true" : "false", v ? 4 : 5);
}

void nx_json_null(nx_json *j)
{
    sep(j);
    put(j, "null", 4);
}

void nx_json_raw(nx_json *j, const char *json, size_t n)
{
    sep(j);
    put(j, json, n);
}

void nx_json_key_str(nx_json *j, const char *key, const char *s)
{
    nx_json_key(j, key);
    nx_json_str(j, s);
}

void nx_json_key_u64(nx_json *j, const char *key, uint64_t v)
{
    nx_json_key(j, key);
    nx_json_u64(j, v);
}

void nx_json_key_bool(nx_json *j, const char *key, bool v)
{
    nx_json_key(j, key);
    nx_json_bool(j, v);
}

void nx_base64_encode(const uint8_t *in, size_t n, char *out)
{
    static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n)
            v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < n)
            v |= in[i + 2];
        out[o++] = A[(v >> 18) & 63];
        out[o++] = A[(v >> 12) & 63];
        out[o++] = i + 1 < n ? A[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? A[v & 63] : '=';
    }
    out[o] = '\0';
}
