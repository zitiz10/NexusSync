/* static.c — see static.h. */
#include "net/static.h"

#include "util/log.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int nx_static_path_ok(const char *p)
{
    if (p[0] != '/' || strlen(p) > 1024)
        return -1;
    if (strchr(p, '\\') || strchr(p, ':'))
        return -1;
    const char *seg = p + 1;
    while (*seg) {
        const char *slash = strchr(seg, '/');
        size_t n = slash ? (size_t)(slash - seg) : strlen(seg);
        if (n == 0 && slash) /* "//" */
            return -1;
        if (n > 0 && seg[0] == '.') /* ".", "..", ".env" */
            return -1;
        if (!slash)
            break;
        seg = slash + 1;
    }
    return 0;
}

const char *nx_static_mime(const char *path)
{
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
    if (!dot || (slash && dot < slash))
        return "application/octet-stream";
    static const struct {
        const char *ext, *type;
    } T[] = {
        {".html", "text/html; charset=utf-8"},
        {".js", "text/javascript; charset=utf-8"},
        {".mjs", "text/javascript; charset=utf-8"},
        {".css", "text/css; charset=utf-8"},
        {".json", "application/json; charset=utf-8"},
        {".map", "application/json; charset=utf-8"},
        {".webmanifest", "application/manifest+json"},
        {".svg", "image/svg+xml"},
        {".png", "image/png"},
        {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"},
        {".ico", "image/x-icon"},
        {".webp", "image/webp"},
        {".woff2", "font/woff2"},
        {".woff", "font/woff"},
        {".wasm", "application/wasm"},
        {".txt", "text/plain; charset=utf-8"},
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++)
        if (strcmp(dot, T[i].ext) == 0)
            return T[i].type;
    return "application/octet-stream";
}

/* Streams one file. Returns 0 if a response was sent. */
static int send_file(nx_ctx *c, const char *fs_path, const char *url_path, const char *cache)
{
    plat_file *f;
    if (plat_is_dir(fs_path) || plat_file_open(fs_path, PLAT_O_READ, &f) != 0)
        return -1;
    uint64_t size;
    if (plat_file_size(f, &size) != 0) {
        plat_file_close(f);
        return -1;
    }
    char extra[128];
    snprintf(extra, sizeof extra, "Cache-Control: %s\r\n", cache);
    char head[2048];
    size_t hl = nx_http_format_head(head, sizeof head, 200, nx_static_mime(url_path), size, extra,
                                    c->keep_alive);
    int rc = hl ? plat_send_all(c->sock, head, hl) : -1;
    c->responded = true;
    if (rc == 0 && !c->head_only) {
        uint8_t *buf = malloc(65536);
        uint64_t off = 0;
        while (rc == 0 && buf && off < size) {
            size_t want = size - off < 65536 ? (size_t)(size - off) : 65536;
            int64_t got = plat_pread(f, buf, want, off);
            if (got <= 0) {
                rc = -1; /* the file shrank under us: the length is already sent */
                break;
            }
            rc = plat_send_all(c->sock, buf, (size_t)got);
            off += (uint64_t)got;
        }
        if (!buf)
            rc = -1;
        free(buf);
    }
    plat_file_close(f);
    if (rc != 0)
        c->keep_alive = false; /* response cut short: the connection is unusable */
    return 0;
}

void nx_static_serve(nx_ctx *c)
{
    const char *m = c->req->method;
    if (strcmp(m, "GET") != 0 && strcmp(m, "HEAD") != 0) {
        nx_reply_error(c, 405, "method_not_allowed", "Only GET and HEAD are allowed here.");
        return;
    }
    const char *web = c->app->cfg.web_root;
    char fs[1700];
    const char *p = c->path;
    if (nx_static_path_ok(p) != 0) {
        nx_reply_error(c, 400, "bad_path", "That path is not allowed.");
        return;
    }
    bool is_root = strcmp(p, "/") == 0 || strcmp(p, "/index.html") == 0;
    if (!is_root) {
        snprintf(fs, sizeof fs, "%s%s", web, p);
        const char *cache = strncmp(p, "/assets/", 8) == 0
                                ? "public, max-age=31536000, immutable" /* hashed names */
                                : "public, max-age=3600";
        if (send_file(c, fs, p, cache) == 0)
            return;
        /* A missing file under /assets/ or with an extension is a real 404;
         * anything else is a client-side route. */
        const char *last = strrchr(p, '/');
        if (strncmp(p, "/assets/", 8) == 0 || (last && strchr(last, '.'))) {
            nx_reply_error(c, 404, "not_found", "No such file.");
            return;
        }
    }
    snprintf(fs, sizeof fs, "%s/index.html", web);
    if (send_file(c, fs, "/index.html", "no-cache") != 0) {
        LOG_ERROR("static", "index.html missing; build the web app (npm run build) web_root=%s",
                  web);
        nx_reply_error(c, 404, "not_found", "The web app is not built on this server.");
    }
}
