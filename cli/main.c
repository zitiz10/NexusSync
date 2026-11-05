/*
 * nexus — the NexusSync command-line client (SPEC §17).
 *
 *   nexus [--server URL] [--json] login <username>
 *   nexus put <file> [--fixed] [--new-file]
 *   nexus get <id|name> [-o out]
 *   nexus ls [--shared]
 *   nexus rm <id>
 *
 * The session (server URL and token) is kept in ~/.nexussync/session with
 * mode 0600 (NEXUS_HOME overrides the directory). The password is asked on
 * the terminal; scripts may set NEXUS_PASSWORD instead.
 *
 * put chunks the file exactly like the browser does (same code as the
 * server), sends the manifest, then only the blocks the server lacks.
 */
#include "client.h"

#include "cJSON.h"
#include "crypto/sha256.h"
#include "engine/chunk.h"
#include "engine/recipe.h"
#include "platform/platform.h"
#include "util/bytes.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool g_json;

static void usage(void)
{
    fprintf(stderr,
            "usage: nexus [--server URL] [--json] login <username>\n"
            "       nexus put <file> [--fixed] [--new-file]\n"
            "       nexus get <id|name> [-o out]\n"
            "       nexus ls [--shared]\n"
            "       nexus rm <id>\n");
}

/* ===== session file ============================================================== */

static void session_path(char *dir, size_t dn, char *file, size_t fn)
{
    const char *h = getenv("NEXUS_HOME");
    if (h && *h)
        snprintf(dir, dn, "%s", h);
    else {
        const char *home = getenv("HOME");
        if (!home || !*home)
            home = getenv("USERPROFILE"); /* Windows */
        snprintf(dir, dn, "%s/.nexussync", home ? home : ".");
    }
    snprintf(file, fn, "%s/session", dir);
}

static int session_load(char *url, size_t un, char *token, size_t tn)
{
    char dir[1024], file[1100];
    session_path(dir, sizeof dir, file, sizeof file);
    plat_file *f;
    if (plat_file_open(file, PLAT_O_READ, &f) != 0)
        return -1;
    char buf[1024];
    int64_t n = plat_pread(f, buf, sizeof buf - 1, 0);
    plat_file_close(f);
    if (n <= 0)
        return -1;
    buf[n] = '\0';
    char *nl = strchr(buf, '\n');
    if (!nl)
        return -1;
    *nl = '\0';
    snprintf(url, un, "%s", buf);
    snprintf(token, tn, "%.*s", (int)strcspn(nl + 1, "\r\n"), nl + 1);
    return 0;
}

static int session_save(const char *url, const char *token)
{
    char dir[1024], file[1100];
    session_path(dir, sizeof dir, file, sizeof file);
    if (plat_mkdir_p(dir) != 0)
        return -1;
    plat_file *f;
    if (plat_file_open(file, PLAT_O_WRITE | PLAT_O_CREATE | PLAT_O_TRUNC | PLAT_O_PRIVATE, &f) != 0)
        return -1;
    char buf[1024];
    int n = snprintf(buf, sizeof buf, "%s\n%s\n", url, token);
    int rc = plat_pwrite_all(f, buf, (size_t)n, 0);
    if (plat_file_close(f) != 0)
        rc = -1;
    return rc;
}

/* ===== helpers ======================================================================== */

/* Prints the server's error message for a failed response. */
static int fail(const nx_response *r, const char *what)
{
    const char *msg = NULL, *code = NULL;
    cJSON *j = r->body ? cJSON_Parse(r->body) : NULL;
    const cJSON *e = j ? cJSON_GetObjectItemCaseSensitive(j, "error") : NULL;
    if (e) {
        const cJSON *m = cJSON_GetObjectItemCaseSensitive(e, "message");
        const cJSON *cd = cJSON_GetObjectItemCaseSensitive(e, "code");
        msg = cJSON_IsString(m) ? m->valuestring : NULL;
        code = cJSON_IsString(cd) ? cd->valuestring : NULL;
    }
    fprintf(stderr, "nexus: %s failed: %s (HTTP %d%s%s)\n", what, msg ? msg : "unexpected response",
            r->status, code ? ", " : "", code ? code : "");
    cJSON_Delete(j);
    return 1;
}

static void human_bytes(uint64_t b, char *out, size_t n)
{
    const char *u[] = {"B", "KB", "MB", "GB", "TB"};
    double v = (double)b;
    int i = 0;
    while (v >= 1024 && i < 4) {
        v /= 1024;
        i++;
    }
    snprintf(out, n, i ? "%.1f %s" : "%.0f %s", v, u[i]);
}

static const char *base_name(const char *p)
{
    const char *b = p;
    for (const char *q = p; *q; q++)
        if (*q == '/' || *q == '\\')
            b = q + 1;
    return b;
}

/* ===== login ============================================================================== */

static int cmd_login(nx_client *c, const char *url, const char *username)
{
    char pw[300];
    const char *env = getenv("NEXUS_PASSWORD");
    if (env)
        snprintf(pw, sizeof pw, "%s", env);
    else if (plat_read_password("Password: ", pw, sizeof pw) < 0)
        return 1;
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "username", username);
    cJSON_AddStringToObject(b, "password", pw);
    char *body = cJSON_PrintUnformatted(b);
    cJSON_Delete(b);
    memset(pw, 0, sizeof pw);
    nx_response r;
    int rc = body ? nx_client_request(c, "POST", "/api/auth/login", "application/json", body,
                                      strlen(body), NULL, NULL, &r)
                  : -1;
    if (body)
        memset(body, 0, strlen(body));
    free(body);
    if (rc != 0) {
        fprintf(stderr, "nexus: cannot reach %s\n", url);
        return 1;
    }
    char token[80];
    if (r.status != 200 || !nx_response_cookie(&r, token, sizeof token)) {
        int f = fail(&r, "login");
        nx_response_free(&r);
        return f;
    }
    nx_response_free(&r);
    if (session_save(url, token) != 0) {
        fprintf(stderr, "nexus: could not save the session file\n");
        return 1;
    }
    printf("Signed in to %s as %s.\n", url, username);
    return 0;
}

/* ===== put ================================================================================ */

typedef struct {
    nx_chunk_ref *refs;
    uint64_t *offs;
    uint32_t n, cap;
    uint64_t pos;
} chunk_list;

static int on_chunk(const uint8_t *data, size_t len, void *ctx)
{
    chunk_list *l = ctx;
    if (l->n == l->cap) {
        uint32_t cap = l->cap ? l->cap * 2 : 1024;
        nx_chunk_ref *r = realloc(l->refs, cap * sizeof *r);
        if (!r)
            return 1;
        l->refs = r;
        uint64_t *o = realloc(l->offs, cap * sizeof *o);
        if (!o)
            return 1;
        l->offs = o;
        l->cap = cap;
    }
    nx_sha256(data, len, l->refs[l->n].hash);
    l->refs[l->n].len = (uint32_t)len;
    l->offs[l->n] = l->pos;
    l->pos += len;
    l->n++;
    return 0;
}

/* Sends the given chunk indices in bodies of up to 4 MB. */
static int send_chunks(nx_client *c, const char *uid, plat_file *f, const chunk_list *l,
                       const uint32_t *idx, size_t n, uint64_t *sent)
{
    const size_t cap = 4u << 20;
    uint8_t *body = malloc(cap + 8 + NX_CHUNK_MAX_SIZE);
    if (!body)
        return -1;
    char path[160];
    snprintf(path, sizeof path, "/api/uploads/%s/chunks", uid);
    size_t i = 0;
    int rc = 0;
    while (i < n && rc == 0) {
        size_t len = 0;
        for (; i < n && len < cap; i++) {
            uint32_t k = idx[i];
            nx_put_u32(body + len, k);
            nx_put_u32(body + len + 4, l->refs[k].len);
            if (plat_pread(f, body + len + 8, l->refs[k].len, l->offs[k]) != (int64_t)l->refs[k].len) {
                fprintf(stderr, "nexus: the file changed while uploading\n");
                rc = -1;
                break;
            }
            len += 8 + l->refs[k].len;
            *sent += l->refs[k].len;
        }
        if (rc != 0)
            break;
        nx_response r;
        if (nx_client_request(c, "POST", path, "application/octet-stream", body, len, NULL, NULL,
                              &r) != 0) {
            fprintf(stderr, "nexus: connection lost while sending blocks\n");
            rc = -1;
            break;
        }
        cJSON *j = r.status == 200 ? cJSON_Parse(r.body) : NULL;
        const cJSON *rej = j ? cJSON_GetObjectItemCaseSensitive(j, "rejected") : NULL;
        if (!j || !cJSON_IsArray(rej) || cJSON_GetArraySize(rej) != 0) {
            if (r.status != 200)
                fail(&r, "sending blocks");
            else
                fprintf(stderr, "nexus: the server rejected some blocks (did the file change?)\n");
            rc = -1;
        }
        cJSON_Delete(j);
        nx_response_free(&r);
    }
    free(body);
    return rc;
}

static int cmd_put(nx_client *c, const char *path, bool fixed, bool new_file)
{
    plat_file *f;
    uint64_t size;
    if (plat_file_open(path, PLAT_O_READ, &f) != 0 || plat_file_size(f, &size) != 0) {
        fprintf(stderr, "nexus: cannot read %s\n", path);
        return 1;
    }
    /* Pass 1: chunk and hash, streaming, like the browser's worker. */
    chunk_list l = {0};
    nx_chunker *ch = malloc(sizeof *ch);
    uint8_t *win = malloc(8u << 20);
    nx_sha256_ctx whole;
    nx_sha256_init(&whole);
    int rc = ch && win ? 0 : -1;
    if (rc == 0) {
        nx_chunker_init(ch, fixed ? NX_CHUNKER_FIXED : NX_CHUNKER_CDC);
        for (uint64_t off = 0; off < size && rc == 0;) {
            size_t want = size - off < (8u << 20) ? (size_t)(size - off) : (8u << 20);
            int64_t got = plat_pread(f, win, want, off);
            if (got <= 0) {
                rc = -1;
                break;
            }
            nx_sha256_update(&whole, win, (size_t)got);
            rc = nx_chunker_push(ch, win, (size_t)got, on_chunk, &l);
            off += (uint64_t)got;
        }
        if (rc == 0)
            rc = nx_chunker_finish(ch, on_chunk, &l);
    }
    free(ch);
    free(win);
    uint8_t sha[32];
    nx_sha256_final(&whole, sha);
    char hex[65];
    nx_hex(sha, 32, hex);
    char *uid = NULL;
    uint32_t *need = NULL;
    size_t nneed = 0;
    uint64_t sent = 0;
    if (rc != 0) {
        fprintf(stderr, "nexus: reading %s failed\n", path);
        goto out;
    }

    /* 1. Start the upload. */
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "name", base_name(path));
    cJSON_AddNumberToObject(b, "size", (double)size);
    cJSON_AddStringToObject(b, "sha256", hex);
    cJSON_AddStringToObject(b, "chunker", fixed ? "fixed" : "cdc");
    cJSON_AddNumberToObject(b, "chunkCount", l.n);
    if (new_file)
        cJSON_AddNullToObject(b, "versionOf");
    char *body = cJSON_PrintUnformatted(b);
    cJSON_Delete(b);
    nx_response r;
    rc = body ? nx_client_request(c, "POST", "/api/uploads", "application/json", body, strlen(body),
                                  NULL, NULL, &r)
              : -1;
    free(body);
    if (rc != 0) {
        fprintf(stderr, "nexus: cannot reach the server\n");
        goto out;
    }
    cJSON *j = r.status == 201 ? cJSON_Parse(r.body) : NULL;
    const cJSON *ju = j ? cJSON_GetObjectItemCaseSensitive(j, "uploadId") : NULL;
    if (!cJSON_IsString(ju)) {
        rc = fail(&r, "starting the upload");
        cJSON_Delete(j);
        nx_response_free(&r);
        goto out;
    }
    uid = malloc(40);
    snprintf(uid, 40, "%s", ju->valuestring);
    cJSON_Delete(j);
    nx_response_free(&r);

    /* 2. Manifest pages: the server answers which blocks it lacks. */
    need = malloc((l.n ? l.n : 1) * sizeof *need);
    uint8_t *page = malloc(8192u * 36u);
    rc = need && page ? 0 : -1;
    for (uint32_t p0 = 0; p0 < l.n && rc == 0; p0 += 8192) {
        uint32_t cnt = l.n - p0 < 8192 ? l.n - p0 : 8192;
        for (uint32_t i = 0; i < cnt; i++) {
            memcpy(page + (size_t)i * 36, l.refs[p0 + i].hash, 32);
            nx_put_u32(page + (size_t)i * 36 + 32, l.refs[p0 + i].len);
        }
        char mp[160];
        snprintf(mp, sizeof mp, "/api/uploads/%s/manifest?page=%u", uid, p0 / 8192);
        if (nx_client_request(c, "POST", mp, "application/octet-stream", page, (size_t)cnt * 36,
                              NULL, NULL, &r) != 0) {
            fprintf(stderr, "nexus: connection lost while sending the manifest\n");
            rc = -1;
            break;
        }
        j = r.status == 200 ? cJSON_Parse(r.body) : NULL;
        const cJSON *jn = j ? cJSON_GetObjectItemCaseSensitive(j, "need") : NULL;
        if (!cJSON_IsArray(jn)) {
            rc = fail(&r, "sending the manifest");
        } else {
            const cJSON *it;
            cJSON_ArrayForEach(it, jn)
            {
                if (cJSON_IsNumber(it) && it->valuedouble >= 0 && it->valuedouble < cnt)
                    need[nneed++] = p0 + (uint32_t)it->valuedouble;
            }
        }
        cJSON_Delete(j);
        nx_response_free(&r);
    }
    free(page);

    /* 3. Blocks the server lacks; 4. commit (resend if it still misses some). */
    if (rc == 0)
        rc = send_chunks(c, uid, f, &l, need, nneed, &sent);
    for (int attempt = 0; attempt < 3 && rc == 0; attempt++) {
        char cp[160];
        snprintf(cp, sizeof cp, "/api/uploads/%s/commit", uid);
        if (nx_client_request(c, "POST", cp, NULL, NULL, 0, NULL, NULL, &r) != 0) {
            fprintf(stderr, "nexus: connection lost during commit\n");
            rc = -1;
            break;
        }
        j = cJSON_Parse(r.body ? r.body : "");
        if (r.status == 201 && j) {
            const cJSON *st = cJSON_GetObjectItemCaseSensitive(j, "stats");
            const cJSON *id = cJSON_GetObjectItemCaseSensitive(j, "id");
            const cJSON *ver = cJSON_GetObjectItemCaseSensitive(j, "version");
            const cJSON *nc = st ? cJSON_GetObjectItemCaseSensitive(st, "newChunks") : NULL;
            const cJSON *rp = st ? cJSON_GetObjectItemCaseSensitive(st, "reusedPercent") : NULL;
            if (g_json) {
                printf("%s\n", r.body);
            } else {
                char hs[32], hz[32];
                human_bytes(sent, hs, sizeof hs);
                human_bytes(size, hz, sizeof hz);
                printf("%s v%d (id %.0f): %u blocks, %d new, sent %s of %s (%.1f%% reused)\n",
                       base_name(path), ver ? ver->valueint : 0, id ? id->valuedouble : 0, l.n,
                       nc ? nc->valueint : 0, hs, hz, rp ? rp->valuedouble : 0.0);
            }
            cJSON_Delete(j);
            nx_response_free(&r);
            goto out;
        }
        const cJSON *e = j ? cJSON_GetObjectItemCaseSensitive(j, "error") : NULL;
        const cJSON *ix = e ? cJSON_GetObjectItemCaseSensitive(e, "indices") : NULL;
        if (r.status == 409 && cJSON_IsArray(ix)) {
            nneed = 0;
            const cJSON *it;
            cJSON_ArrayForEach(it, ix)
            {
                if (cJSON_IsNumber(it) && it->valuedouble >= 0 && it->valuedouble < l.n)
                    need[nneed++] = (uint32_t)it->valuedouble;
            }
            rc = send_chunks(c, uid, f, &l, need, nneed, &sent);
        } else {
            rc = fail(&r, "commit");
        }
        cJSON_Delete(j);
        nx_response_free(&r);
    }
    if (rc == 0)
        rc = -1; /* still missing blocks after three rounds */
out:
    plat_file_close(f);
    free(l.refs);
    free(l.offs);
    free(need);
    free(uid);
    return rc == 0 ? 0 : 1;
}

/* ===== get ================================================================================ */

typedef struct {
    plat_file *f;
    uint64_t off;
    nx_sha256_ctx sha;
} sink_ctx;

static int to_file(const uint8_t *data, size_t len, void *ctx)
{
    sink_ctx *s = ctx;
    if (plat_pwrite_all(s->f, data, len, s->off) != 0)
        return 1;
    nx_sha256_update(&s->sha, data, len);
    s->off += len;
    return 0;
}

static bool all_digits(const char *s)
{
    if (!*s)
        return false;
    for (; *s; s++)
        if (*s < '0' || *s > '9')
            return false;
    return true;
}

/* Finds the newest own file called name. */
static int find_by_name(nx_client *c, const char *name, char *id, size_t n)
{
    char q[1024], *p = q;
    p += snprintf(q, sizeof q, "/api/files?scope=mine&limit=200&q=");
    for (const unsigned char *s = (const unsigned char *)name; *s && p < q + sizeof q - 4; s++)
        p += (*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') ||
                     *s == '.' || *s == '-' || *s == '_'
                 ? snprintf(p, 2, "%c", *s)
                 : snprintf(p, 4, "%%%02X", *s);
    nx_response r;
    if (nx_client_request(c, "GET", q, NULL, NULL, 0, NULL, NULL, &r) != 0)
        return -1;
    int rc = -1;
    cJSON *j = r.status == 200 ? cJSON_Parse(r.body) : NULL;
    const cJSON *items = j ? cJSON_GetObjectItemCaseSensitive(j, "items") : NULL, *it;
    cJSON_ArrayForEach(it, items)
    {
        const cJSON *nm = cJSON_GetObjectItemCaseSensitive(it, "name");
        const cJSON *jid = cJSON_GetObjectItemCaseSensitive(it, "id");
        if (cJSON_IsString(nm) && strcmp(nm->valuestring, name) == 0 && cJSON_IsNumber(jid)) {
            snprintf(id, n, "%.0f", jid->valuedouble);
            rc = 0;
            break; /* newest first */
        }
    }
    if (r.status != 200)
        fail(&r, "finding the file");
    cJSON_Delete(j);
    nx_response_free(&r);
    return rc;
}

static int cmd_get(nx_client *c, const char *what, const char *out)
{
    char id[32];
    if (all_digits(what))
        snprintf(id, sizeof id, "%s", what);
    else if (find_by_name(c, what, id, sizeof id) != 0) {
        fprintf(stderr, "nexus: no file named %s\n", what);
        return 1;
    }
    char path[96];
    snprintf(path, sizeof path, "/api/files/%s", id);
    nx_response r;
    if (nx_client_request(c, "GET", path, NULL, NULL, 0, NULL, NULL, &r) != 0) {
        fprintf(stderr, "nexus: cannot reach the server\n");
        return 1;
    }
    cJSON *j = r.status == 200 ? cJSON_Parse(r.body) : NULL;
    const cJSON *jn = j ? cJSON_GetObjectItemCaseSensitive(j, "name") : NULL,
                *js = j ? cJSON_GetObjectItemCaseSensitive(j, "sha256") : NULL,
                *jz = j ? cJSON_GetObjectItemCaseSensitive(j, "size") : NULL;
    if (!cJSON_IsString(jn) || !cJSON_IsString(js) || !cJSON_IsNumber(jz)) {
        int f = fail(&r, "reading the file details");
        cJSON_Delete(j);
        nx_response_free(&r);
        return f;
    }
    char dest[1100], part[1200], want[65];
    snprintf(dest, sizeof dest, "%s", out ? out : base_name(jn->valuestring));
    snprintf(want, sizeof want, "%s", js->valuestring);
    uint64_t size = (uint64_t)jz->valuedouble;
    cJSON_Delete(j);
    nx_response_free(&r);
    if (!out && plat_path_exists(dest)) {
        fprintf(stderr, "nexus: %s exists; use -o to choose where to save\n", dest);
        return 1;
    }
    snprintf(part, sizeof part, "%s.part", dest);
    sink_ctx s = {0};
    if (plat_file_open(part, PLAT_O_WRITE | PLAT_O_CREATE | PLAT_O_TRUNC, &s.f) != 0) {
        fprintf(stderr, "nexus: cannot write %s\n", part);
        return 1;
    }
    nx_sha256_init(&s.sha);
    snprintf(path, sizeof path, "/api/files/%s/content", id);
    int rc = nx_client_request(c, "GET", path, NULL, NULL, 0, to_file, &s, &r);
    bool ok = rc == 0 && r.status == 200 && s.off == size;
    if (ok && plat_fsync_file(s.f) != 0)
        ok = false;
    plat_file_close(s.f);
    uint8_t got[32];
    nx_sha256_final(&s.sha, got);
    char hex[65];
    nx_hex(got, 32, hex);
    if (ok && strcmp(hex, want) != 0) {
        fprintf(stderr, "nexus: SHA-256 mismatch: the download is damaged and was discarded\n");
        ok = false;
    } else if (!ok) {
        fprintf(stderr, "nexus: the download did not complete (HTTP %d)\n", r.status);
    }
    if (!ok || plat_rename_replace(part, dest) != 0) {
        plat_remove(part);
        return 1;
    }
    if (g_json)
        printf("{\"id\":%s,\"path\":\"%s\",\"size\":%" PRIu64 ",\"sha256\":\"%s\"}\n", id, dest, size,
               hex);
    else {
        char hs[32];
        human_bytes(size, hs, sizeof hs);
        printf("Saved %s (%s), SHA-256 verified.\n", dest, hs);
    }
    return 0;
}

static double jnum(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : 0;
}

static const char *jstr(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : "?";
}

/* ===== ls / rm =============================================================================== */

static int cmd_ls(nx_client *c, bool shared)
{
    char path[160], cursor[32] = "";
    bool first = true;
    if (g_json)
        printf("[");
    else
        printf("%8s %4s %10s %8s  %-6s %s\n", "ID", "VER", "SIZE", "REUSED", "SHARED", "NAME");
    for (;;) {
        snprintf(path, sizeof path, "/api/files?limit=200%s%s%s", shared ? "&scope=shared" : "",
                 cursor[0] ? "&cursor=" : "", cursor);
        nx_response r;
        if (nx_client_request(c, "GET", path, NULL, NULL, 0, NULL, NULL, &r) != 0) {
            fprintf(stderr, "nexus: cannot reach the server\n");
            return 1;
        }
        cJSON *j = r.status == 200 ? cJSON_Parse(r.body) : NULL;
        const cJSON *items = j ? cJSON_GetObjectItemCaseSensitive(j, "items") : NULL, *it;
        if (!cJSON_IsArray(items)) {
            int f = fail(&r, "listing files");
            cJSON_Delete(j);
            nx_response_free(&r);
            return f;
        }
        cJSON_ArrayForEach(it, items)
        {
            if (g_json) {
                char *s = cJSON_PrintUnformatted(it);
                printf("%s%s", first ? "" : ",", s ? s : "null");
                free(s);
            } else {
                char hs[32];
                human_bytes((uint64_t)jnum(it, "size"), hs, sizeof hs);
                printf("%8.0f %4.0f %10s %7.1f%%  %-6s %s\n", jnum(it, "id"), jnum(it, "version"), hs,
                       jnum(it, "reusedPercent"),
                       cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(it, "shared")) ? "yes" : "no",
                       jstr(it, "name"));
            }
            first = false;
        }
        const cJSON *nc = cJSON_GetObjectItemCaseSensitive(j, "nextCursor");
        bool more = cJSON_IsString(nc);
        if (more)
            snprintf(cursor, sizeof cursor, "%s", nc->valuestring);
        cJSON_Delete(j);
        nx_response_free(&r);
        if (!more)
            break;
    }
    if (g_json)
        printf("]\n");
    return 0;
}

static int cmd_rm(nx_client *c, const char *id)
{
    if (!all_digits(id)) {
        fprintf(stderr, "nexus: rm takes a file id (see nexus ls)\n");
        return 1;
    }
    char path[96];
    snprintf(path, sizeof path, "/api/files/%s", id);
    nx_response r;
    if (nx_client_request(c, "DELETE", path, NULL, NULL, 0, NULL, NULL, &r) != 0) {
        fprintf(stderr, "nexus: cannot reach the server\n");
        return 1;
    }
    int rc = r.status == 204 ? 0 : fail(&r, "delete");
    nx_response_free(&r);
    if (rc == 0 && !g_json)
        printf("Deleted file %s.\n", id);
    return rc;
}

/* ===== main ================================================================================= */

int main(int argc, char **argv)
{
    const char *server = NULL, *out = NULL;
    bool fixed = false, new_file = false, shared = false;
    const char *args[4];
    int na = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--server") == 0 && i + 1 < argc)
            server = argv[++i];
        else if (strcmp(argv[i], "--json") == 0)
            g_json = true;
        else if (strcmp(argv[i], "--fixed") == 0)
            fixed = true;
        else if (strcmp(argv[i], "--new-file") == 0)
            new_file = true;
        else if (strcmp(argv[i], "--shared") == 0)
            shared = true;
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
            out = argv[++i];
        else if (argv[i][0] == '-' && argv[i][1]) {
            usage();
            return 2;
        } else if (na < 4)
            args[na++] = argv[i];
        else {
            usage();
            return 2;
        }
    }
    if (na == 0) {
        usage();
        return 2;
    }
    if (plat_net_init() != 0)
        return 1;
    char url[512] = "", token[80] = "";
    bool have_session = session_load(url, sizeof url, token, sizeof token) == 0;
    if (server)
        snprintf(url, sizeof url, "%s", server);
    if (!url[0]) {
        fprintf(stderr, "nexus: no server yet; use --server http://<ip>:8080 login <username>\n");
        return 2;
    }
    nx_client c;
    if (nx_client_init(&c, url) != 0) {
        fprintf(stderr, "nexus: the server must look like http://192.168.1.20:8080\n");
        return 2;
    }
    if (have_session && (!server || strcmp(server, url) == 0))
        snprintf(c.token, sizeof c.token, "%s", token);
    int rc;
    const char *cmd = args[0];
    if (strcmp(cmd, "login") == 0 && na == 2)
        rc = cmd_login(&c, url, args[1]);
    else if (!c.token[0] && strcmp(cmd, "login") != 0) {
        fprintf(stderr, "nexus: not signed in; run nexus --server %s login <username>\n", url);
        rc = 2;
    } else if (strcmp(cmd, "put") == 0 && na == 2)
        rc = cmd_put(&c, args[1], fixed, new_file);
    else if (strcmp(cmd, "get") == 0 && na == 2)
        rc = cmd_get(&c, args[1], out);
    else if (strcmp(cmd, "ls") == 0 && na == 1)
        rc = cmd_ls(&c, shared);
    else if (strcmp(cmd, "rm") == 0 && na == 2)
        rc = cmd_rm(&c, args[1]);
    else {
        usage();
        rc = 2;
    }
    nx_client_close(&c);
    plat_net_cleanup();
    return rc;
}
