/* client.c — see client.h. */
#include "client.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_BODY_IN_MEMORY (64u << 20)

int nx_client_init(nx_client *c, const char *url)
{
    memset(c, 0, sizeof *c);
    c->s = PLAT_SOCK_INVALID;
    if (strncmp(url, "http://", 7) != 0)
        return -1;
    const char *h = url + 7;
    size_t hl = strcspn(h, ":/");
    if (hl == 0 || hl >= sizeof c->host)
        return -1;
    memcpy(c->host, h, hl);
    c->host[hl] = '\0';
    c->port = 8080;
    if (h[hl] == ':') {
        char *end;
        unsigned long p = strtoul(h + hl + 1, &end, 10);
        if (p == 0 || p > 65535 || (*end && *end != '/'))
            return -1;
        c->port = (uint16_t)p;
    }
    return 0;
}

void nx_client_close(nx_client *c)
{
    if (c->connected)
        plat_sock_close(c->s);
    c->connected = false;
}

static int connect_now(nx_client *c)
{
    nx_client_close(c);
    if (plat_tcp_connect(c->host, c->port, &c->s) != 0)
        return -1;
    plat_sock_set_timeouts(c->s, 120000, 120000); /* a commit re-reads the file */
    c->connected = true;
    return 0;
}

void nx_response_free(nx_response *r)
{
    free(r->body);
    r->body = NULL;
}

bool nx_response_cookie(const nx_response *r, char *out, size_t cap)
{
    const char *p = strstr(r->head, "\r\nSet-Cookie: nx_session=");
    if (!p)
        return false;
    p += strlen("\r\nSet-Cookie: nx_session=");
    size_t n = strcspn(p, ";\r");
    if (n == 0 || n >= cap)
        return false;
    memcpy(out, p, n);
    out[n] = '\0';
    return true;
}

/* Header value (case-insensitive name) inside a response head. */
static const char *head_value(const char *head, const char *name, char *out, size_t cap)
{
    size_t nl = strlen(name);
    for (const char *p = strstr(head, "\r\n"); p; p = strstr(p + 2, "\r\n")) {
        const char *line = p + 2;
        bool same = true;
        for (size_t i = 0; i < nl && same; i++) {
            char a = line[i], b = name[i];
            if (a >= 'A' && a <= 'Z')
                a = (char)(a + 32);
            same = a == b;
        }
        if (same && line[nl] == ':') {
            const char *v = line + nl + 1;
            while (*v == ' ')
                v++;
            size_t n = strcspn(v, "\r");
            if (n >= cap)
                n = cap - 1;
            memcpy(out, v, n);
            out[n] = '\0';
            return out;
        }
    }
    return NULL;
}

/* One attempt on the current connection. *got_any reports whether any
 * response byte arrived (if not, a retry on a new connection is safe). */
static int attempt(nx_client *c, const char *head, size_t hl, const void *body, size_t len,
                   nx_body_cb sink, void *ctx, nx_response *r, bool *got_any)
{
    *got_any = false;
    if (plat_send_all(c->s, head, hl) != 0 || (len && plat_send_all(c->s, body, len) != 0))
        return -1;
    size_t have = 0;
    char *end = NULL;
    char buf[65536];
    size_t extra = 0;
    while (!end) {
        if (have >= sizeof r->head - 1)
            return -1;
        int64_t n = plat_recv(c->s, r->head + have, sizeof r->head - 1 - have);
        if (n <= 0)
            return -1;
        *got_any = true;
        have += (size_t)n;
        r->head[have] = '\0';
        end = strstr(r->head, "\r\n\r\n");
    }
    size_t head_len = (size_t)(end - r->head) + 4;
    extra = have - head_len;
    memcpy(buf, r->head + head_len, extra); /* body bytes read with the head */
    r->head[head_len] = '\0';
    if (sscanf(r->head, "HTTP/1.%*d %d", &r->status) != 1)
        return -1;
    char v[64];
    uint64_t clen = 0;
    if (head_value(r->head, "content-length", v, sizeof v))
        clen = strtoull(v, NULL, 10);
    bool close_after = head_value(r->head, "connection", v, sizeof v) && strcmp(v, "close") == 0;
    if (!sink) {
        if (clen > MAX_BODY_IN_MEMORY)
            return -1;
        r->body = malloc((size_t)clen + 1);
        if (!r->body)
            return -1;
    }
    uint64_t got = 0;
    size_t chunk = extra < clen ? extra : (size_t)clen;
    for (;;) {
        if (chunk) {
            if (sink) {
                if (sink((const uint8_t *)buf, chunk, ctx) != 0)
                    return -1;
            } else {
                memcpy(r->body + got, buf, chunk);
            }
            got += chunk;
        }
        if (got >= clen)
            break;
        uint64_t want = clen - got < sizeof buf ? clen - got : sizeof buf;
        int64_t n = plat_recv(c->s, buf, (size_t)want);
        if (n <= 0)
            return -1; /* short body: e.g. the server stopped a corrupt download */
        chunk = (size_t)n;
    }
    if (r->body) {
        r->body[got] = '\0';
        r->body_len = (size_t)got;
    }
    if (close_after)
        nx_client_close(c);
    return 0;
}

int nx_client_request(nx_client *c, const char *method, const char *path, const char *ctype,
                      const void *body, size_t len, nx_body_cb sink, void *ctx, nx_response *r)
{
    memset(r, 0, sizeof *r);
    char head[2048];
    char cookie[128] = "";
    if (c->token[0])
        snprintf(cookie, sizeof cookie, "Cookie: nx_session=%s\r\n", c->token);
    char ct[128] = "";
    if (ctype)
        snprintf(ct, sizeof ct, "Content-Type: %s\r\n", ctype);
    int hl = snprintf(head, sizeof head,
                      "%s %s HTTP/1.1\r\nHost: %s:%u\r\nUser-Agent: nexus-cli\r\n"
                      "X-NexusSync: 1\r\n%s%sContent-Length: %zu\r\n\r\n",
                      method, path, c->host, (unsigned)c->port, cookie, ct, len);
    if (hl < 0 || (size_t)hl >= sizeof head)
        return -1;
    /* The server drops keep-alive connections idle for 5 s: do not race it. */
    if (c->connected && plat_now_mono_ms() - c->last_used_ms > 3000)
        nx_client_close(c);
    for (int tries = 0; tries < 2; tries++) {
        bool reused = c->connected;
        if (!c->connected && connect_now(c) != 0)
            return -1;
        bool got_any;
        int rc = attempt(c, head, (size_t)hl, body, len, sink, ctx, r, &got_any);
        c->last_used_ms = plat_now_mono_ms();
        if (rc == 0)
            return 0;
        nx_client_close(c);
        nx_response_free(r);
        if (!reused || got_any)
            return -1; /* only a stale reused connection is worth a retry */
    }
    return -1;
}
