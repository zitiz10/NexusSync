/* config.c — see config.h. */
#include "util/config.h"

#include "engine/chunk.h"
#include "platform/platform.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void nx_config_defaults(nx_config *c)
{
    memset(c, 0, sizeof *c);
    snprintf(c->listen_host, sizeof c->listen_host, "0.0.0.0");
    c->listen_port = 8080;
    snprintf(c->data_dir, sizeof c->data_dir, "./data");
    snprintf(c->web_root, sizeof c->web_root, "./web/dist");
    c->workers = 8;
    c->max_connections = 64;
    c->chunker = NX_CHUNKER_CDC;
    c->container_size = 4194304;
    c->buffer_pool_pages = 4096;
    c->checkpoint_interval_s = 60;
    c->gc_grace_s = 3600;
    c->verify_on_read = true;
    c->verify_on_commit = true;
    c->upload_session_ttl_s = 1800;
    c->session_ttl_s = 43200;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r'))
        s[--n] = '\0';
    return s;
}

static int parse_u64(const char *v, uint64_t min, uint64_t max, uint64_t *out)
{
    if (!*v || *v == '-' || *v == '+')
        return -1;
    char *end;
    errno = 0;
    unsigned long long x = strtoull(v, &end, 10);
    if (errno || *end || x < min || x > max)
        return -1;
    *out = x;
    return 0;
}

static int parse_bool(const char *v, bool *out)
{
    if (strcmp(v, "true") == 0 || strcmp(v, "yes") == 0 || strcmp(v, "1") == 0)
        *out = true;
    else if (strcmp(v, "false") == 0 || strcmp(v, "no") == 0 || strcmp(v, "0") == 0)
        *out = false;
    else
        return -1;
    return 0;
}

static int set_string(char *dst, size_t cap, const char *v)
{
    if (strlen(v) >= cap || !*v)
        return -1;
    memcpy(dst, v, strlen(v) + 1);
    return 0;
}

/* "host:port" or ":port". */
static int parse_listen(const char *v, nx_config *c)
{
    const char *colon = strrchr(v, ':');
    if (!colon)
        return -1;
    size_t hl = (size_t)(colon - v);
    uint64_t port;
    if (hl >= sizeof c->listen_host || parse_u64(colon + 1, 0, 65535, &port) != 0)
        return -1;
    memcpy(c->listen_host, v, hl);
    c->listen_host[hl] = '\0';
    if (hl == 0)
        snprintf(c->listen_host, sizeof c->listen_host, "0.0.0.0");
    c->listen_port = (uint16_t)port;
    return 0;
}

static int set_key(nx_config *c, const char *k, const char *v, const char **why)
{
    uint64_t x;
    *why = "invalid value";
    if (strcmp(k, "listen") == 0)
        return parse_listen(v, c);
    if (strcmp(k, "data_dir") == 0)
        return set_string(c->data_dir, sizeof c->data_dir, v);
    if (strcmp(k, "web_root") == 0)
        return set_string(c->web_root, sizeof c->web_root, v);
    if (strcmp(k, "workers") == 0) {
        *why = "workers must be 1..256";
        if (parse_u64(v, 1, 256, &x) != 0)
            return -1;
        c->workers = (uint32_t)x;
        return 0;
    }
    if (strcmp(k, "max_connections") == 0) {
        *why = "max_connections must be 1..4096";
        if (parse_u64(v, 1, 4096, &x) != 0)
            return -1;
        c->max_connections = (uint32_t)x;
        return 0;
    }
    if (strcmp(k, "chunker") == 0) {
        *why = "chunker must be cdc or fixed";
        if (strcmp(v, "cdc") == 0)
            c->chunker = NX_CHUNKER_CDC;
        else if (strcmp(v, "fixed") == 0)
            c->chunker = NX_CHUNKER_FIXED;
        else
            return -1;
        return 0;
    }
    if (strcmp(k, "container_size") == 0) {
        *why = "container_size must be 1 MB..4 GB";
        if (parse_u64(v, 1u << 20, UINT32_MAX, &x) != 0)
            return -1;
        c->container_size = x;
        return 0;
    }
    if (strcmp(k, "buffer_pool_pages") == 0) {
        *why = "buffer_pool_pages must be 64..4194304";
        if (parse_u64(v, 64, 4194304, &x) != 0)
            return -1;
        c->buffer_pool_pages = (uint32_t)x;
        return 0;
    }
    struct {
        const char *name;
        uint32_t *dst;
        uint64_t min, max;
    } nums[] = {
        {"checkpoint_interval_s", &c->checkpoint_interval_s, 1, 86400},
        {"gc_grace_s", &c->gc_grace_s, 0, 30u * 86400},
        {"upload_session_ttl_s", &c->upload_session_ttl_s, 60, 7u * 86400},
        {"session_ttl_s", &c->session_ttl_s, 60, 30u * 86400},
    };
    for (size_t i = 0; i < sizeof nums / sizeof nums[0]; i++)
        if (strcmp(k, nums[i].name) == 0) {
            if (parse_u64(v, nums[i].min, nums[i].max, &x) != 0)
                return -1;
            *nums[i].dst = (uint32_t)x;
            return 0;
        }
    if (strcmp(k, "verify_on_read") == 0)
        return parse_bool(v, &c->verify_on_read);
    if (strcmp(k, "verify_on_commit") == 0)
        return parse_bool(v, &c->verify_on_commit);
    if (strcmp(k, "join_code") == 0) {
        *why = "join_code must be 8 characters from 23456789ABCDEFGHJKMNPQRSTUVWXYZ";
        if (strlen(v) != 8)
            return -1;
        for (const char *q = v; *q; q++) {
            char u = *q >= 'a' && *q <= 'z' ? (char)(*q - 32) : *q;
            if (!strchr("23456789ABCDEFGHJKMNPQRSTUVWXYZ", u))
                return -1;
        }
        return set_string(c->join_code, sizeof c->join_code, v);
    }
    *why = "unknown key";
    return -1;
}

int nx_config_parse(const char *text, nx_config *c, char *err, size_t err_len)
{
    int line_no = 0;
    const char *p = text;
    while (*p) {
        line_no++;
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        char line[1024];
        if (n >= sizeof line) {
            snprintf(err, err_len, "line %d: too long", line_no);
            return -1;
        }
        memcpy(line, p, n);
        line[n] = '\0';
        p = nl ? nl + 1 : p + n;
        char *hash = strchr(line, '#');
        if (hash)
            *hash = '\0';
        char *s = trim(line);
        if (!*s)
            continue;
        char *eq = strchr(s, '=');
        if (!eq) {
            snprintf(err, err_len, "line %d: expected key = value", line_no);
            return -1;
        }
        *eq = '\0';
        char *k = trim(s), *v = trim(eq + 1);
        const char *why;
        if (set_key(c, k, v, &why) != 0) {
            snprintf(err, err_len, "line %d: %s: %s", line_no, k, why);
            return -1;
        }
    }
    return 0;
}

int nx_config_load(const char *path, nx_config *c, char *err, size_t err_len)
{
    plat_file *f;
    if (plat_file_open(path, PLAT_O_READ, &f) != 0) {
        snprintf(err, err_len, "cannot open %s: %s", path, plat_last_error());
        return -1;
    }
    uint64_t size = 0;
    if (plat_file_size(f, &size) != 0 || size > 1u << 20) {
        plat_file_close(f);
        snprintf(err, err_len, "cannot read %s", path);
        return -1;
    }
    char *buf = malloc((size_t)size + 1);
    int rc = -1;
    if (buf && plat_pread(f, buf, (size_t)size, 0) == (int64_t)size) {
        buf[size] = '\0';
        rc = memchr(buf, '\0', (size_t)size) ? -1 : nx_config_parse(buf, c, err, err_len);
        if (rc != 0 && memchr(buf, '\0', (size_t)size))
            snprintf(err, err_len, "%s contains a NUL byte", path);
    } else {
        snprintf(err, err_len, "cannot read %s", path);
    }
    plat_file_close(f);
    free(buf);
    return rc;
}
