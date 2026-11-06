/* compare.c — comparing two files block by block. See compare.h, D-043. */
#include "api/compare.h"

#include "api/files.h"
#include "crypto/sha256.h"
#include "engine/chunk.h"
#include "util/hmap.h"
#include "util/log.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One computed comparison. Immutable once built, shared by readers through
 * a reference count (the cache may drop it while a request still uses it). */
typedef struct {
    uint64_t a_id, b_id;
    int mode; /* enum nx_chunker_kind */
    uint64_t made_ms;
    int refs; /* under the cache mutex */

    nx_chunk_ref *a, *b;
    uint32_t na, nb;
    bool a_stored, b_stored; /* blocks came from the recipe: they have locations */
    int64_t *b_match;        /* per b block: first a index with that hash, or -1 */
    int64_t *a_match;        /* per a block: first b index with that hash, or -1 */
    uint64_t a_bytes, b_bytes, reused, sent_bytes;
    int64_t first_diff; /* first b block not in a, or -1 */
} cmp;

struct nx_compare {
    pthread_mutex_t mu; /* leaf lock (SPEC §9): never held across an engine call */
    cmp *slot[NX_COMPARE_CACHE];
};

static void cmp_free(cmp *r)
{
    if (!r)
        return;
    free(r->a);
    free(r->b);
    free(r->a_match);
    free(r->b_match);
    free(r);
}

int nx_compare_create(nx_compare **out)
{
    nx_compare *c = calloc(1, sizeof *c);
    if (!c)
        return -1;
    pthread_mutex_init(&c->mu, NULL);
    *out = c;
    return 0;
}

void nx_compare_destroy(nx_compare *c)
{
    if (!c)
        return;
    for (unsigned i = 0; i < NX_COMPARE_CACHE; i++)
        cmp_free(c->slot[i]);
    pthread_mutex_destroy(&c->mu);
    free(c);
}

/* ---- cache ------------------------------------------------------------------------ */

static cmp *cache_get(nx_compare *c, uint64_t a, uint64_t b, int mode)
{
    uint64_t now = plat_now_mono_ms();
    cmp *hit = NULL;
    pthread_mutex_lock(&c->mu);
    for (unsigned i = 0; i < NX_COMPARE_CACHE; i++) {
        cmp *r = c->slot[i];
        if (!r)
            continue;
        if (now - r->made_ms > NX_COMPARE_TTL_MS) { /* expired: drop it */
            c->slot[i] = NULL;
            if (r->refs == 0)
                cmp_free(r);
            else
                r->refs = -r->refs; /* orphaned: the last reader frees it */
            continue;
        }
        if (r->a_id == a && r->b_id == b && r->mode == mode) {
            r->refs++;
            hit = r;
        }
    }
    pthread_mutex_unlock(&c->mu);
    return hit;
}

/* Puts r in the cache (replacing the oldest entry) and keeps one reference
 * for the caller. */
static void cache_put(nx_compare *c, cmp *r)
{
    pthread_mutex_lock(&c->mu);
    unsigned victim = 0;
    for (unsigned i = 0; i < NX_COMPARE_CACHE; i++) {
        if (!c->slot[i]) {
            victim = i;
            break;
        }
        if (c->slot[i]->made_ms < c->slot[victim]->made_ms)
            victim = i;
    }
    cmp *old = c->slot[victim];
    if (old) {
        if (old->refs == 0)
            cmp_free(old);
        else
            old->refs = -old->refs; /* orphaned: the last reader frees it */
    }
    r->refs = 1;
    c->slot[victim] = r;
    pthread_mutex_unlock(&c->mu);
}

static void cache_release(nx_compare *c, cmp *r)
{
    pthread_mutex_lock(&c->mu);
    if (r->refs > 0)
        r->refs--;
    else if (++r->refs == 0) /* orphaned and this was the last reader */
        cmp_free(r);
    pthread_mutex_unlock(&c->mu);
}

/* ---- building a comparison -------------------------------------------------------------- */

typedef struct {
    nx_chunker ch;
    nx_chunk_ref *v;
    uint32_t n, cap;
} rechunk;

static int on_block(const uint8_t *data, size_t len, void *ctx)
{
    rechunk *r = ctx;
    if (r->n == r->cap) {
        uint32_t cap = r->cap ? r->cap * 2 : 1024;
        nx_chunk_ref *nv = realloc(r->v, (size_t)cap * sizeof *nv);
        if (!nv)
            return -1;
        r->v = nv;
        r->cap = cap;
    }
    nx_sha256(data, len, r->v[r->n].hash);
    r->v[r->n].len = (uint32_t)len;
    r->n++;
    return 0;
}

static int on_bytes(const uint8_t *data, size_t len, void *ctx)
{
    rechunk *r = ctx;
    return nx_chunker_push(&r->ch, data, len, on_block, r);
}

/* The blocks of f cut with `mode`: from the recipe when it was stored that
 * way, else by reading the file through the engine and cutting it. Returns
 * 0, NX_ENGINE_CORRUPT (damaged stored block), or -1. */
static int file_blocks(nx_engine *e, const nx_file *f, int mode, nx_chunk_ref **out, uint32_t *n,
                       bool *stored)
{
    *out = NULL;
    *n = 0;
    if (f->chunker == mode) {
        nx_file meta;
        int rc = nx_engine_file_recipe(e, f->id, &meta, out);
        if (rc != 0)
            return rc == NX_ENGINE_CORRUPT ? rc : -1;
        *n = meta.chunk_count;
        *stored = true;
        return 0;
    }
    rechunk *r = malloc(sizeof *r); /* nx_chunker holds a 16 KB buffer: not on the stack */
    if (!r)
        return -1;
    r->v = NULL;
    r->n = r->cap = 0;
    nx_chunker_init(&r->ch, (enum nx_chunker_kind)mode);
    int rc = nx_engine_read_file(e, f->id, on_bytes, r);
    if (rc == 0)
        rc = nx_chunker_finish(&r->ch, on_block, r);
    if (rc != 0) {
        free(r->v);
        free(r);
        return rc == NX_ENGINE_CORRUPT ? rc : -1;
    }
    *out = r->v;
    *n = r->n;
    *stored = false;
    free(r);
    return 0;
}

/* For each block of `of`, the first index of the same hash in `in`. */
static int match_into(const nx_chunk_ref *in, uint32_t nin, const nx_chunk_ref *of, uint32_t nof,
                      int64_t **out)
{
    *out = malloc((nof ? nof : 1) * sizeof **out);
    nx_hmap32 first;
    if (!*out || nx_hmap32_init(&first, nin ? nin : 1) != 0) {
        free(*out);
        *out = NULL;
        return -1;
    }
    int rc = 0;
    for (uint32_t i = 0; i < nin && rc == 0; i++) {
        bool fresh;
        uint64_t *v = nx_hmap32_upsert(&first, in[i].hash, &fresh);
        if (!v)
            rc = -1;
        else if (fresh)
            *v = i;
    }
    for (uint32_t i = 0; i < nof && rc == 0; i++) {
        uint64_t *v = nx_hmap32_get(&first, of[i].hash);
        (*out)[i] = v ? (int64_t)*v : -1;
    }
    nx_hmap32_free(&first);
    if (rc != 0) {
        free(*out);
        *out = NULL;
    }
    return rc;
}

static int build(nx_engine *e, const nx_file *fa, const nx_file *fb, int mode, cmp **out)
{
    cmp *r = calloc(1, sizeof *r);
    if (!r)
        return -1;
    r->a_id = fa->id;
    r->b_id = fb->id;
    r->mode = mode;
    int rc = file_blocks(e, fa, mode, &r->a, &r->na, &r->a_stored);
    if (rc == 0)
        rc = file_blocks(e, fb, mode, &r->b, &r->nb, &r->b_stored);
    if (rc == 0 && (match_into(r->a, r->na, r->b, r->nb, &r->b_match) != 0 ||
                    match_into(r->b, r->nb, r->a, r->na, &r->a_match) != 0))
        rc = -1;
    if (rc != 0) {
        cmp_free(r);
        return rc;
    }
    for (uint32_t i = 0; i < r->na; i++)
        r->a_bytes += r->a[i].len;
    /* A new block repeated inside version 2 is sent once: count distinct. */
    nx_hmap32 sent;
    if (nx_hmap32_init(&sent, 64) != 0) {
        cmp_free(r);
        return -1;
    }
    r->first_diff = -1;
    for (uint32_t i = 0; i < r->nb && rc == 0; i++) {
        r->b_bytes += r->b[i].len;
        if (r->b_match[i] >= 0) {
            r->reused++;
            continue;
        }
        if (r->first_diff < 0)
            r->first_diff = i;
        bool fresh;
        if (!nx_hmap32_upsert(&sent, r->b[i].hash, &fresh))
            rc = -1;
        else if (fresh)
            r->sent_bytes += r->b[i].len;
    }
    nx_hmap32_free(&sent);
    if (rc != 0) {
        cmp_free(r);
        return -1;
    }
    r->made_ms = plat_now_mono_ms();
    *out = r;
    return 0;
}

/* ---- the handler ----------------------------------------------------------------------- */

static bool parse_id(const char *query, const char *key, uint64_t *out)
{
    char v[32];
    if (!nx_query_get(query, key, v, sizeof v) || v[0] < '1' || v[0] > '9')
        return false;
    char *end;
    unsigned long long n = strtoull(v, &end, 10);
    if (*end || n == UINT64_MAX)
        return false;
    *out = n;
    return true;
}

/* Optional whole number in [0, max]; *present false when the key is absent. */
static bool parse_uint(const char *query, const char *key, uint64_t max, uint64_t *out, bool *present)
{
    char v[32];
    *present = nx_query_get(query, key, v, sizeof v);
    if (!*present)
        return true;
    if (v[0] < '0' || v[0] > '9')
        return false;
    char *end;
    unsigned long long n = strtoull(v, &end, 10);
    if (*end || n > max)
        return false;
    *out = n;
    return true;
}

static void json_block(nx_json *j, nx_engine *e, const nx_chunk_ref *blk, uint32_t index, int64_t match,
                       bool stored)
{
    char hex[65];
    nx_hex(blk->hash, 32, hex);
    nx_json_obj_open(j);
    nx_json_key_u64(j, "index", index);
    nx_json_key_u64(j, "size", blk->len);
    nx_json_key_str(j, "sha256", hex);
    nx_json_key_bool(j, "reused", match >= 0);
    nx_json_key(j, "matchIndex");
    if (match >= 0)
        nx_json_u64(j, (uint64_t)match);
    else
        nx_json_null(j);
    nx_json_key(j, "stored");
    nx_index_entry ent;
    if (stored && nx_engine_lookup(e, blk->hash, &ent) == 0) {
        nx_json_obj_open(j);
        nx_json_key_u64(j, "containerId", ent.container_id);
        nx_json_key_u64(j, "offset", ent.offset);
        nx_json_key_u64(j, "sharedBy", ent.refcount);
        nx_json_obj_close(j);
    } else {
        nx_json_null(j);
    }
    nx_json_obj_close(j);
}

static void json_file(nx_json *j, const char *key, const nx_file *f)
{
    nx_json_key(j, key);
    nx_json_obj_open(j);
    nx_json_key_u64(j, "id", f->id);
    nx_json_key_str(j, "name", f->name);
    nx_json_key_u64(j, "version", f->version);
    nx_json_key_u64(j, "size", f->size);
    nx_json_obj_close(j);
}

void nx_h_compare(nx_ctx *c)
{
    nx_app *app = c->app;
    nx_engine *e = app->engine;
    const char *q = c->req->query;
    uint64_t a_id, b_id, from = 0, count = 32;
    bool has_from, has_count;
    char mode_s[16];
    if (!parse_id(q, "a", &a_id) || !parse_id(q, "b", &b_id)) {
        nx_reply_error(c, 400, "bad_files", "Give two file ids: a (version 1) and b (version 2).");
        return;
    }
    int mode = -1;
    if (!nx_query_get(q, "mode", mode_s, sizeof mode_s) || strcmp(mode_s, "cdc") == 0)
        mode = NX_CHUNKER_CDC;
    else if (strcmp(mode_s, "fixed") == 0)
        mode = NX_CHUNKER_FIXED;
    if (mode < 0) {
        nx_reply_error(c, 400, "bad_mode", "mode must be fixed or cdc.");
        return;
    }
    if (!parse_uint(q, "from", UINT32_MAX, &from, &has_from) ||
        !parse_uint(q, "count", NX_COMPARE_MAX_WINDOW, &count, &has_count) || (has_count && count == 0)) {
        nx_reply_error(c, 400, "bad_window", "from must be a block index and count 1 to 64.");
        return;
    }
    /* Both files must be readable by the caller, every time (cache or not). */
    nx_file fa, fb;
    nx_catalog *cat = nx_engine_catalog(e);
    if (!nx_catalog_file_by_id(cat, a_id, &fa) || !nx_file_can_read(&c->user, &fa) ||
        !nx_catalog_file_by_id(cat, b_id, &fb) || !nx_file_can_read(&c->user, &fb)) {
        nx_reply_error(c, 404, "not_found", "No such file.");
        return;
    }
    if (fa.size > NX_COMPARE_MAX_FILE || fb.size > NX_COMPARE_MAX_FILE) {
        nx_reply_error(c, 422, "too_large_to_compare", "Files over 1 GB cannot be compared on this screen.");
        return;
    }

    cmp *r = cache_get(app->compare, a_id, b_id, mode);
    if (!r) {
        uint64_t t0 = plat_now_mono_ms();
        int rc = build(e, &fa, &fb, mode, &r);
        if (rc != 0) {
            if (rc == NX_ENGINE_CORRUPT)
                nx_reply_error(c, 500, "stored_block_damaged",
                               "A stored block failed its check; run an integrity scan.");
            else
                nx_reply_error(c, 500, "internal", "The comparison could not be computed.");
            return;
        }
        LOG_INFO("compare", "computed a=%" PRIu64 " b=%" PRIu64 " mode=%s blocks=%" PRIu32 "+%" PRIu32
                 " ms=%" PRIu64,
                 a_id, b_id, mode == NX_CHUNKER_CDC ? "cdc" : "fixed", r->na, r->nb,
                 plat_now_mono_ms() - t0);
        cache_put(app->compare, r);
    }

    uint32_t longest = r->na > r->nb ? r->na : r->nb;
    if (!has_from) /* DESIGN §8.5: start 3 blocks before the first difference */
        from = r->first_diff > 3 ? (uint64_t)r->first_diff - 3 : 0;
    if (longest == 0 || from >= longest)
        from = longest > count ? longest - count : 0;

    nx_json j;
    nx_json_init(&j);
    nx_json_obj_open(&j);
    nx_json_key_str(&j, "mode", mode == NX_CHUNKER_CDC ? "cdc" : "fixed");
    nx_json_key(&j, "files");
    nx_json_obj_open(&j);
    json_file(&j, "a", &fa);
    json_file(&j, "b", &fb);
    nx_json_obj_close(&j);
    nx_json_key(&j, "totals");
    nx_json_obj_open(&j);
    nx_json_key(&j, "a");
    nx_json_obj_open(&j);
    nx_json_key_u64(&j, "chunks", r->na);
    nx_json_key_u64(&j, "bytes", r->a_bytes);
    nx_json_obj_close(&j);
    nx_json_key(&j, "b");
    nx_json_obj_open(&j);
    nx_json_key_u64(&j, "chunks", r->nb);
    nx_json_key_u64(&j, "bytes", r->b_bytes);
    nx_json_key_u64(&j, "reused", r->reused);
    nx_json_key_u64(&j, "sentBytes", r->sent_bytes);
    nx_json_obj_close(&j);
    nx_json_obj_close(&j);
    nx_json_key(&j, "firstDifference");
    if (r->first_diff >= 0)
        nx_json_u64(&j, (uint64_t)r->first_diff);
    else
        nx_json_null(&j);

    nx_json_key(&j, "window");
    nx_json_obj_open(&j);
    nx_json_key_u64(&j, "from", from);
    nx_json_key_u64(&j, "count", count);
    nx_json_key(&j, "a");
    nx_json_arr_open(&j);
    for (uint64_t i = from; i < from + count && i < r->na; i++)
        json_block(&j, e, &r->a[i], (uint32_t)i, r->a_match[i], r->a_stored);
    nx_json_arr_close(&j);
    nx_json_key(&j, "b");
    nx_json_arr_open(&j);
    for (uint64_t i = from; i < from + count && i < r->nb; i++)
        json_block(&j, e, &r->b[i], (uint32_t)i, r->b_match[i], r->b_stored);
    nx_json_arr_close(&j);
    nx_json_obj_close(&j);

    /* Minimap of version 2: [[1|0, run], ...], 1 = already in version 1. */
    nx_json_key(&j, "minimap");
    nx_json_arr_open(&j);
    for (uint32_t i = 0; i < r->nb;) {
        int v = r->b_match[i] >= 0;
        uint32_t k = i;
        while (k < r->nb && (r->b_match[k] >= 0) == v)
            k++;
        nx_json_arr_open(&j);
        nx_json_u64(&j, (uint64_t)v);
        nx_json_u64(&j, k - i);
        nx_json_arr_close(&j);
        i = k;
    }
    nx_json_arr_close(&j);
    nx_json_obj_close(&j);
    cache_release(app->compare, r);
    nx_reply_json(c, 200, &j, "Cache-Control: no-store\r\n");
    nx_json_free(&j);
}
