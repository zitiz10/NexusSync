/* files.c — the files API. See files.h. */
#include "api/files.h"

#include "cJSON.h"
#include "crypto/sha256.h"
#include "util/hmap.h"
#include "util/log.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ===== shared helpers ========================================================= */

double nx_reused_percent(uint64_t size, uint64_t bytes_sent)
{
    if (size == 0)
        return 100.0;
    uint64_t sent = bytes_sent < size ? bytes_sent : size;
    double p = 100.0 * (double)(size - sent) / (double)size;
    return (double)(int64_t)(p * 10.0 + 0.5) / 10.0; /* one decimal */
}

bool nx_file_can_read(const nx_user *u, const nx_file *f)
{
    if (f->flags & NX_FILE_DELETED)
        return false;
    return u->role == NX_ROLE_ADMIN || f->owner_id == u->id || (f->flags & NX_FILE_SHARED);
}

bool nx_file_can_write(const nx_user *u, const nx_file *f)
{
    if (f->flags & NX_FILE_DELETED)
        return false;
    return u->role == NX_ROLE_ADMIN || f->owner_id == u->id;
}

void nx_json_file_open(nx_json *j, nx_engine *e, const nx_file *f)
{
    nx_user owner;
    bool has_owner = nx_catalog_user_by_id(nx_engine_catalog(e), f->owner_id, &owner);
    char sha[65], at[40];
    nx_hex(f->sha256, 32, sha);
    plat_format_utc_iso(f->created_at, at, sizeof at);
    nx_json_obj_open(j);
    nx_json_key_u64(j, "id", f->id);
    nx_json_key_str(j, "name", f->name);
    nx_json_key_u64(j, "version", f->version);
    nx_json_key(j, "prevVersionId");
    if (f->prev_version_id)
        nx_json_u64(j, f->prev_version_id);
    else
        nx_json_null(j);
    nx_json_key_u64(j, "ownerId", f->owner_id);
    nx_json_key_str(j, "ownerName", has_owner ? owner.display : "");
    nx_json_key_u64(j, "size", f->size);
    nx_json_key_u64(j, "chunkCount", f->chunk_count);
    nx_json_key_str(j, "chunker", f->chunker == 0 ? "fixed" : "cdc");
    nx_json_key_str(j, "sha256", sha);
    nx_json_key_u64(j, "bytesSent", f->bytes_sent);
    nx_json_key(j, "reusedPercent");
    nx_json_f64(j, nx_reused_percent(f->size, f->bytes_sent));
    nx_json_key_str(j, "createdAt", at);
    nx_json_key_bool(j, "shared", (f->flags & NX_FILE_SHARED) != 0);
}

/* Loads a file the caller may read. On failure replies 404 (also when the
 * caller may not see it: no hint that it exists) and returns false. */
static bool load_readable(nx_ctx *c, nx_file *f)
{
    if (!nx_catalog_file_by_id(nx_engine_catalog(c->app->engine), c->id, f) ||
        !nx_file_can_read(&c->user, f)) {
        nx_reply_error(c, 404, "not_found", "No such file.");
        return false;
    }
    return true;
}

/* ===== GET /api/files ============================================================== */

typedef struct {
    const nx_user *me;
    bool mine, shared;
    char q[256];
    const nx_hmap64 *superseded;
} list_filter;

static bool ci_contains(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    if (n == 0)
        return true;
    for (const char *h = hay; *h; h++) {
        size_t k = 0;
        for (; k < n && h[k]; k++) {
            char a = h[k], b = needle[k];
            if (a >= 'A' && a <= 'Z')
                a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z')
                b = (char)(b + 32);
            if (a != b)
                break;
        }
        if (k == n)
            return true;
    }
    return false;
}

static bool list_keep(const nx_file *f, void *ctx)
{
    const list_filter *lf = ctx;
    if (!nx_file_can_read(lf->me, f) || nx_hmap64_get(lf->superseded, f->id, NULL))
        return false;
    if (lf->mine && f->owner_id != lf->me->id)
        return false;
    if (lf->shared && !(f->flags & NX_FILE_SHARED))
        return false;
    return ci_contains(f->name, lf->q);
}

static int g_sort; /* qsort has no context argument; set under the sort mutex */
static pthread_mutex_t g_sort_mu = PTHREAD_MUTEX_INITIALIZER;

static int ci_cmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a;
        char y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (x != y || !x)
            return (unsigned char)x - (unsigned char)y;
    }
}

static int cmp_files(const void *pa, const void *pb)
{
    const nx_file *a = pa, *b = pb;
    int r = 0;
    switch (g_sort) {
    case 1: /* name */
        r = ci_cmp(a->name, b->name);
        break;
    case 2: /* size, largest first */
        r = a->size < b->size ? 1 : a->size > b->size ? -1 : 0;
        break;
    case 3: /* shared first */
        r = (int)((b->flags & NX_FILE_SHARED) != 0) - (int)((a->flags & NX_FILE_SHARED) != 0);
        break;
    default:
        break;
    }
    if (r == 0) /* then newest first; ids grow with time and break ties */
        r = a->id < b->id ? 1 : a->id > b->id ? -1 : 0;
    return r;
}

static bool has_prev(const nx_file *f, void *ctx)
{
    (void)ctx;
    return f->prev_version_id != 0;
}

void nx_h_files_list(nx_ctx *c)
{
    nx_engine *e = c->app->engine;
    nx_catalog *cat = nx_engine_catalog(e);
    list_filter lf = {.me = &c->user};
    char v[64];
    if (nx_query_get(c->req->query, "q", lf.q, sizeof lf.q) == false)
        lf.q[0] = '\0';
    int sort = 0;
    if (nx_query_get(c->req->query, "sort", v, sizeof v)) {
        if (strcmp(v, "name") == 0)
            sort = 1;
        else if (strcmp(v, "size") == 0)
            sort = 2;
        else if (strcmp(v, "shared") == 0)
            sort = 3;
        else if (strcmp(v, "newest") != 0) {
            nx_reply_error(c, 400, "invalid_sort", "sort is newest, name, size, or shared.");
            return;
        }
    }
    if (nx_query_get(c->req->query, "scope", v, sizeof v)) {
        if (strcmp(v, "mine") == 0)
            lf.mine = true;
        else if (strcmp(v, "shared") == 0)
            lf.shared = true;
        else {
            nx_reply_error(c, 400, "invalid_scope", "scope is mine or shared.");
            return;
        }
    }
    uint64_t limit = 50, offset = 0;
    char *end;
    if (nx_query_get(c->req->query, "limit", v, sizeof v)) {
        limit = strtoull(v, &end, 10);
        if (!v[0] || *end || limit == 0 || limit > 200) {
            nx_reply_error(c, 400, "invalid_limit", "limit is 1 to 200.");
            return;
        }
    }
    if (nx_query_get(c->req->query, "cursor", v, sizeof v) && v[0]) {
        offset = strtoull(v, &end, 10);
        if (*end || v[0] == '-') {
            nx_reply_error(c, 400, "invalid_cursor", "Use the nextCursor of the previous page.");
            return;
        }
    }
    /* Files that a newer version points back at are not chain heads. */
    nx_file *older;
    size_t nolder;
    nx_hmap64 superseded;
    if (nx_catalog_files(cat, has_prev, NULL, &older, &nolder) != 0 ||
        nx_hmap64_init(&superseded, nolder + 16) != 0) {
        nx_reply_error(c, 500, "internal", "Out of memory.");
        return;
    }
    for (size_t i = 0; i < nolder; i++)
        nx_hmap64_put(&superseded, older[i].prev_version_id, 1);
    free(older);
    lf.superseded = &superseded;
    nx_file *fv;
    size_t n;
    if (nx_catalog_files(cat, list_keep, &lf, &fv, &n) != 0) {
        nx_hmap64_free(&superseded);
        nx_reply_error(c, 500, "internal", "Out of memory.");
        return;
    }
    nx_hmap64_free(&superseded);
    pthread_mutex_lock(&g_sort_mu);
    g_sort = sort;
    qsort(fv, n, sizeof *fv, cmp_files);
    pthread_mutex_unlock(&g_sort_mu);

    nx_json j;
    nx_json_init(&j);
    nx_json_obj_open(&j);
    nx_json_key(&j, "items");
    nx_json_arr_open(&j);
    size_t stop = offset < n ? (size_t)(offset + limit < n ? offset + limit : n) : 0;
    for (size_t i = offset < n ? (size_t)offset : 0; i < stop; i++) {
        nx_json_file_open(&j, e, &fv[i]);
        nx_json_obj_close(&j);
    }
    nx_json_arr_close(&j);
    nx_json_key(&j, "nextCursor");
    if (stop < n && stop > 0) {
        char cur[24];
        snprintf(cur, sizeof cur, "%zu", stop);
        nx_json_str(&j, cur);
    } else {
        nx_json_null(&j);
    }
    nx_json_obj_close(&j);
    nx_reply_json(c, 200, &j, "Cache-Control: no-store\r\n");
    nx_json_free(&j);
    free(fv);
}

/* ===== GET /api/files/{id} ============================================================ */

typedef struct {
    uint64_t id;
} child_ctx;

static bool is_child(const nx_file *f, void *ctx)
{
    return f->prev_version_id == ((child_ctx *)ctx)->id && !(f->flags & NX_FILE_DELETED);
}

void nx_h_file_get(nx_ctx *c)
{
    nx_engine *e = c->app->engine;
    nx_catalog *cat = nx_engine_catalog(e);
    nx_file f;
    if (!load_readable(c, &f))
        return;
    /* The version chain: back to the first version, then forward. */
    enum { MAX_CHAIN = 256 };
    nx_file *chain = malloc(MAX_CHAIN * sizeof *chain); /* ~100 KB: not on the stack */
    if (!chain) {
        nx_reply_error(c, 500, "internal", "Out of memory.");
        return;
    }
    size_t nc = 0;
    nx_file cur = f;
    while (cur.prev_version_id && nc < 128) {
        nx_file p;
        if (!nx_catalog_file_by_id(cat, cur.prev_version_id, &p) || (p.flags & NX_FILE_DELETED))
            break;
        cur = p;
        nc++;
    }
    nc = 0;
    chain[nc++] = cur;
    while (nc < MAX_CHAIN) {
        child_ctx cc = {chain[nc - 1].id};
        nx_file *kids;
        size_t nk;
        if (nx_catalog_files(cat, is_child, &cc, &kids, &nk) != 0)
            break;
        if (nk == 0) {
            free(kids);
            break;
        }
        chain[nc++] = kids[nk - 1]; /* if several, follow the newest */
        free(kids);
    }
    /* Blocks: unique to this file (no other file references them) or shared. */
    nx_file meta;
    nx_chunk_ref *chunks = NULL;
    uint64_t unique = 0;
    int rr = nx_engine_file_recipe(e, f.id, &meta, &chunks);
    if (rr == 0) {
        nx_hmap32 cnt;
        if (nx_hmap32_init(&cnt, meta.chunk_count) == 0) {
            for (uint32_t i = 0; i < meta.chunk_count; i++) {
                uint64_t *v = nx_hmap32_upsert(&cnt, chunks[i].hash, NULL);
                if (v)
                    (*v)++;
            }
            for (uint32_t i = 0; i < meta.chunk_count; i++) {
                uint64_t *v = nx_hmap32_get(&cnt, chunks[i].hash);
                nx_index_entry ent;
                if (v && nx_engine_lookup(e, chunks[i].hash, &ent) == 0 && ent.refcount == *v)
                    unique++;
            }
            nx_hmap32_free(&cnt);
        }
        free(chunks);
    }
    nx_json j;
    nx_json_init(&j);
    nx_json_file_open(&j, e, &f);
    nx_json_key(&j, "versions");
    nx_json_arr_open(&j);
    for (size_t i = nc; i-- > 0;) { /* newest first */
        char at[40];
        plat_format_utc_iso(chain[i].created_at, at, sizeof at);
        nx_json_obj_open(&j);
        nx_json_key_u64(&j, "id", chain[i].id);
        nx_json_key_u64(&j, "version", chain[i].version);
        nx_json_key_u64(&j, "size", chain[i].size);
        nx_json_key_str(&j, "createdAt", at);
        nx_json_obj_close(&j);
    }
    nx_json_arr_close(&j);
    nx_json_key(&j, "blocks");
    nx_json_obj_open(&j);
    nx_json_key_u64(&j, "total", f.chunk_count);
    nx_json_key_u64(&j, "uniqueToFile", unique);
    nx_json_key_u64(&j, "shared", f.chunk_count - unique);
    nx_json_obj_close(&j);
    nx_json_obj_close(&j);
    nx_reply_json(c, 200, &j, "Cache-Control: no-store\r\n");
    nx_json_free(&j);
    free(chain);
}

/* ===== GET /api/files/{id}/content ===================================================== */

static int send_block(const uint8_t *data, size_t len, void *ctx)
{
    nx_ctx *c = ctx;
    return plat_send_all(c->sock, data, len) == 0 ? 0 : 1;
}

/* RFC 8187 encoding for filename*=UTF-8''... */
static void disposition(const char *name, char *out, size_t cap)
{
    size_t o = (size_t)snprintf(out, cap, "Content-Disposition: attachment; filename*=UTF-8''");
    for (const unsigned char *p = (const unsigned char *)name; *p && o + 4 < cap; p++) {
        bool plain = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                     (*p >= '0' && *p <= '9') || strchr("!#$&+-.^_`|~", *p);
        if (plain)
            out[o++] = (char)*p;
        else
            o += (size_t)snprintf(out + o, cap - o, "%%%02X", *p);
    }
    snprintf(out + o, cap - o, "\r\nCache-Control: no-store\r\n");
}

void nx_h_file_content(nx_ctx *c)
{
    nx_file f;
    if (!load_readable(c, &f))
        return;
    char extra[1200];
    disposition(f.name, extra, sizeof extra);
    char head[2048];
    size_t hl = nx_http_format_head(head, sizeof head, 200, "application/octet-stream", f.size, extra,
                                    c->keep_alive);
    c->responded = true;
    if (!hl || plat_send_all(c->sock, head, hl) != 0) {
        c->keep_alive = false;
        return;
    }
    if (c->head_only)
        return;
    /* Streamed block by block: memory stays small for any file size. Each
     * block is re-hashed (verify_on_read) and the whole file is checked. */
    int rc = nx_engine_read_file(c->app->engine, f.id, send_block, c);
    if (rc != 0) {
        /* The length is already promised: closing the connection is the only
         * honest signal. The client sees a short body and its hash check fails. */
        c->keep_alive = false;
        if (rc == NX_ENGINE_CORRUPT) {
            char data[96];
            snprintf(data, sizeof data, "{\"fileId\":%" PRIu64 ",\"name\":null}", f.id);
            nx_hub_publish(c->app->hub, "engine.corruption", data, (nx_audience){NX_AUD_ADMINS, 0});
            LOG_ERROR("files", "download stopped: corrupt data file_id=%" PRIu64, f.id);
        }
    }
}

/* ===== PATCH and DELETE /api/files/{id} ==================================================== */

void nx_h_file_patch(nx_ctx *c)
{
    nx_engine *e = c->app->engine;
    nx_file f;
    if (!load_readable(c, &f))
        return;
    if (!nx_file_can_write(&c->user, &f)) {
        nx_reply_error(c, 403, "forbidden", "Only the owner or the lab admin can change this file.");
        return;
    }
    const char *ct = nx_http_get_header(c->req, "content-type");
    cJSON *b = ct && strncmp(ct, "application/json", 16) == 0 && c->body_len
                   ? cJSON_ParseWithLength((const char *)c->body, c->body_len)
                   : NULL;
    const cJSON *sh = b ? cJSON_GetObjectItemCaseSensitive(b, "shared") : NULL;
    if (!sh || !cJSON_IsBool(sh)) {
        cJSON_Delete(b);
        nx_reply_error(c, 400, "invalid_body", "Send {\"shared\": true|false}.");
        return;
    }
    uint8_t flags = cJSON_IsTrue(sh) ? (uint8_t)(f.flags | NX_FILE_SHARED)
                                     : (uint8_t)(f.flags & ~NX_FILE_SHARED);
    cJSON_Delete(b);
    if (flags != f.flags && nx_engine_set_file_flags(e, f.id, flags) != 0) {
        nx_reply_error(c, 500, "internal", "The change could not be saved.");
        return;
    }
    f.flags = flags;
    nx_json j;
    nx_json_init(&j);
    nx_json_file_open(&j, e, &f);
    nx_json_obj_close(&j);
    nx_reply_json(c, 200, &j, NULL);
    /* Everyone learns about sharing changes (their lists change too). */
    if (flags & NX_FILE_SHARED)
        nx_hub_publish(c->app->hub, "file.updated", j.buf, (nx_audience){NX_AUD_ALL, 0});
    else {
        char data[64];
        snprintf(data, sizeof data, "{\"id\":%" PRIu64 ",\"shared\":false}", f.id);
        nx_hub_publish(c->app->hub, "file.updated", data, (nx_audience){NX_AUD_ALL, 0});
    }
    nx_json_free(&j);
}

void nx_h_file_delete(nx_ctx *c)
{
    nx_file f;
    if (!load_readable(c, &f))
        return;
    if (!nx_file_can_write(&c->user, &f)) {
        nx_reply_error(c, 403, "forbidden", "Only the owner or the lab admin can delete this file.");
        return;
    }
    int rc = nx_engine_delete_file(c->app->engine, f.id);
    if (rc == NX_ENGINE_NOT_FOUND) {
        nx_reply_error(c, 404, "not_found", "No such file.");
        return;
    }
    if (rc != 0) {
        nx_reply_error(c, 500, "internal", "The file could not be deleted.");
        return;
    }
    LOG_INFO("files", "deleted file_id=%" PRIu64 " by user_id=%" PRIu32, f.id, c->user.id);
    nx_reply_empty(c, 204, NULL);
    char data[48];
    snprintf(data, sizeof data, "{\"id\":%" PRIu64 "}", f.id);
    nx_audience aud = (f.flags & NX_FILE_SHARED) ? (nx_audience){NX_AUD_ALL, 0}
                                                 : (nx_audience){NX_AUD_USER, f.owner_id};
    nx_hub_publish(c->app->hub, "file.deleted", data, aud);
}
