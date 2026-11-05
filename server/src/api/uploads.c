/* uploads.c — upload sessions. See uploads.h. */
#include "api/uploads.h"

#include "api/auth.h"
#include "api/files.h"
#include "cJSON.h"
#include "crypto/sha256.h"
#include "engine/chunk.h"
#include "util/bytes.h"
#include "util/hmap.h"
#include "util/log.h"

#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct up {
    struct up *next;
    char id[33];
    uint32_t owner;
    char name[NX_FILENAME_MAX + 1];
    uint64_t size;
    uint8_t sha[32];
    uint8_t chunker;
    uint32_t count, pages;
    uint64_t version_of;

    pthread_mutex_t mu; /* everything below; taken after the table lock */
    nx_chunk_ref *chunks;
    uint8_t *have;      /* per chunk index: the block is known to be stored */
    uint8_t *page_done; /* manifest page received */
    uint32_t pages_done;
    nx_hmap32 first;    /* hash -> first declared index */
    uint64_t bytes_sent;
    uint32_t new_chunks;
    bool committing;

    /* Under the table lock: */
    uint64_t last_ms;
    int refs;
    bool dead;
} up;

struct nx_uploads {
    pthread_mutex_t mu;
    up *list;
    size_t count;
    uint64_t chunk_total; /* declared chunks over all sessions */
};

int nx_uploads_create(nx_uploads **out)
{
    nx_uploads *u = calloc(1, sizeof *u);
    if (!u)
        return -1;
    pthread_mutex_init(&u->mu, NULL);
    *out = u;
    return 0;
}

static void free_up(up *s)
{
    nx_hmap32_free(&s->first);
    free(s->chunks);
    free(s->have);
    free(s->page_done);
    pthread_mutex_destroy(&s->mu);
    free(s);
}

void nx_uploads_destroy(nx_uploads *u)
{
    if (!u)
        return;
    up *s = u->list;
    while (s) {
        up *n = s->next;
        free_up(s);
        s = n;
    }
    pthread_mutex_destroy(&u->mu);
    free(u);
}

/* Caller holds the table lock. */
static void unlink_up(nx_uploads *u, up *s)
{
    for (up **pp = &u->list; *pp; pp = &(*pp)->next)
        if (*pp == s) {
            *pp = s->next;
            break;
        }
    u->count--;
    u->chunk_total -= s->count;
    s->dead = true;
    if (s->refs == 0)
        free_up(s);
}

size_t nx_uploads_expire(nx_uploads *u, uint32_t ttl_s)
{
    uint64_t now = plat_now_mono_ms();
    size_t n = 0;
    pthread_mutex_lock(&u->mu);
    up *s = u->list;
    while (s) {
        up *next = s->next;
        if (s->refs == 0 && now - s->last_ms > (uint64_t)ttl_s * 1000u) {
            unlink_up(u, s);
            n++;
        }
        s = next;
    }
    pthread_mutex_unlock(&u->mu);
    return n;
}

size_t nx_uploads_count(nx_uploads *u)
{
    pthread_mutex_lock(&u->mu);
    size_t n = u->count;
    pthread_mutex_unlock(&u->mu);
    return n;
}

bool nx_uploads_declares(nx_uploads *u, const uint8_t hash[32])
{
    bool found = false;
    pthread_mutex_lock(&u->mu);
    for (up *s = u->list; s && !found; s = s->next) {
        pthread_mutex_lock(&s->mu);
        found = nx_hmap32_get(&s->first, hash) != NULL;
        pthread_mutex_unlock(&s->mu);
    }
    pthread_mutex_unlock(&u->mu);
    return found;
}

/* Finds the caller's session and takes a reference. */
static up *acquire(nx_uploads *u, const char *id, uint32_t owner)
{
    pthread_mutex_lock(&u->mu);
    up *s = u->list;
    while (s && strcmp(s->id, id) != 0)
        s = s->next;
    if (s && s->owner != owner)
        s = NULL; /* someone else's upload looks exactly like a missing one */
    if (s) {
        s->refs++;
        s->last_ms = plat_now_mono_ms();
    }
    pthread_mutex_unlock(&u->mu);
    return s;
}

static void release(nx_uploads *u, up *s)
{
    pthread_mutex_lock(&u->mu);
    s->refs--;
    s->last_ms = plat_now_mono_ms();
    if (s->dead && s->refs == 0)
        free_up(s);
    pthread_mutex_unlock(&u->mu);
}

/* ===== helpers ================================================================== */

static cJSON *json_body(nx_ctx *c)
{
    const char *ct = nx_http_get_header(c->req, "content-type");
    if (!ct || strncmp(ct, "application/json", 16) != 0) {
        nx_reply_error(c, 415, "unsupported_media_type", "Send JSON (Content-Type: application/json).");
        return NULL;
    }
    cJSON *j = c->body_len ? cJSON_ParseWithLength((const char *)c->body, c->body_len) : NULL;
    if (!j || !cJSON_IsObject(j)) {
        cJSON_Delete(j);
        nx_reply_error(c, 400, "invalid_json", "The request body is not a JSON object.");
        return NULL;
    }
    return j;
}

/* A JSON number that is a whole number in [0, max]. */
static bool json_uint(const cJSON *v, uint64_t max, uint64_t *out)
{
    if (!cJSON_IsNumber(v))
        return false;
    double d = v->valuedouble;
    if (!(d >= 0) || d > (double)max || floor(d) != d)
        return false;
    *out = (uint64_t)d;
    return true;
}

/* File names: 1..255 bytes of UTF-8, no control characters, no path
 * separators, not "." or "..". */
static bool name_ok(const char *n)
{
    size_t len = strlen(n);
    if (len == 0 || len > NX_FILENAME_MAX || !nx_utf8_valid(n, len))
        return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)n[i];
        if (ch < 0x20 || ch == 0x7f || ch == '/' || ch == '\\')
            return false;
    }
    return strcmp(n, ".") != 0 && strcmp(n, "..") != 0;
}

/* Whether a chunk length is possible for this chunker at this position. */
static bool len_ok(uint8_t chunker, uint32_t len, bool last)
{
    if (len == 0 || len > NX_CHUNK_MAX_SIZE)
        return false;
    if (chunker == NX_CHUNKER_FIXED)
        return last ? len <= NX_FIXED_SIZE : len == NX_FIXED_SIZE;
    return last || len >= NX_CDC_MIN; /* only the final CDC block can be short */
}

static void emit(nx_app *app, uint32_t owner, const char *type, nx_json *data)
{
    if (!data->oom)
        nx_hub_publish(app->hub, type, data->buf, (nx_audience){NX_AUD_USER, owner});
}

static void emit_progress(nx_app *app, const up *s, const char *phase, uint64_t done)
{
    nx_json j;
    nx_json_init(&j);
    nx_json_obj_open(&j);
    nx_json_key_str(&j, "uploadId", s->id);
    nx_json_key_str(&j, "fileName", s->name);
    nx_json_key_str(&j, "phase", phase);
    nx_json_key_u64(&j, "done", done);
    nx_json_key_u64(&j, "total", s->size);
    nx_json_key(&j, "newTiles");
    nx_json_arr_open(&j);
    nx_json_arr_close(&j);
    nx_json_obj_close(&j);
    emit(app, s->owner, "upload.progress", &j);
    nx_json_free(&j);
}

/* ===== POST /api/uploads ========================================================== */

void nx_h_upload_create(nx_ctx *c)
{
    nx_app *app = c->app;
    cJSON *b = json_body(c);
    if (!b)
        return;
    const cJSON *jn = cJSON_GetObjectItemCaseSensitive(b, "name"),
                *js = cJSON_GetObjectItemCaseSensitive(b, "size"),
                *jh = cJSON_GetObjectItemCaseSensitive(b, "sha256"),
                *jc = cJSON_GetObjectItemCaseSensitive(b, "chunker"),
                *jk = cJSON_GetObjectItemCaseSensitive(b, "chunkCount"),
                *jv = cJSON_GetObjectItemCaseSensitive(b, "versionOf");
    uint64_t size, count, vof = 0;
    uint8_t sha[32], chunker = 0;
    const char *err = NULL, *msg = NULL;
    if (!cJSON_IsString(jn) || !name_ok(jn->valuestring))
        err = "invalid_name", msg = "File names are 1 to 255 bytes, without / or \\.";
    else if (!json_uint(js, NX_UPLOAD_MAX_SIZE, &size))
        err = "invalid_size", msg = "Files can be up to 8 GiB.";
    else if (!cJSON_IsString(jh) || strlen(jh->valuestring) != 64 ||
             nx_unhex(jh->valuestring, sha, 32) != 0)
        err = "invalid_sha256", msg = "sha256 must be 64 hex characters.";
    else if (!cJSON_IsString(jc) || (strcmp(jc->valuestring, "cdc") != 0 &&
                                     strcmp(jc->valuestring, "fixed") != 0))
        err = "invalid_chunker", msg = "chunker must be \"cdc\" or \"fixed\".";
    else if (!json_uint(jk, UINT32_MAX, &count))
        err = "invalid_chunk_count", msg = "chunkCount must be a whole number.";
    else if (jv && !cJSON_IsNull(jv) && (!json_uint(jv, UINT64_MAX - 1, &vof) || vof == 0))
        err = "invalid_version_of", msg = "versionOf must be a file id or null.";
    if (!err) {
        chunker = strcmp(jc->valuestring, "cdc") == 0 ? NX_CHUNKER_CDC : NX_CHUNKER_FIXED;
        /* The chunk count has to fit the size: no block is empty or over
         * 16 KB; fixed blocks are 4 KB; CDC blocks are >= 2 KB but the last. */
        uint64_t lo, hi;
        if (chunker == NX_CHUNKER_FIXED)
            lo = hi = (size + NX_FIXED_SIZE - 1) / NX_FIXED_SIZE;
        else {
            lo = (size + NX_CDC_MAX - 1) / NX_CDC_MAX;
            hi = size ? (size - 1) / NX_CDC_MIN + 1 : 0;
        }
        if (count < lo || count > hi)
            err = "invalid_chunk_count", msg = "chunkCount does not fit the file size.";
        if (jv && cJSON_IsNull(jv))
            vof = NX_VERSION_NONE;
    }
    if (!err && vof && vof != NX_VERSION_NONE) {
        nx_file pf;
        if (!nx_catalog_file_by_id(nx_engine_catalog(app->engine), vof, &pf) ||
            pf.owner_id != c->user.id || (pf.flags & NX_FILE_DELETED))
            err = "version_of_not_found", msg = "versionOf names none of your files.";
    }
    if (err) {
        nx_reply_error(c, 400, err, msg);
        cJSON_Delete(b);
        return;
    }

    up *s = calloc(1, sizeof *s);
    uint32_t pages = (uint32_t)((count + NX_MANIFEST_PAGE - 1) / NX_MANIFEST_PAGE);
    if (!s || nx_hmap32_init(&s->first, count < 1024 ? 1024 : (size_t)count) != 0) {
        free(s);
        cJSON_Delete(b);
        nx_reply_error(c, 500, "internal", "Out of memory.");
        return;
    }
    s->chunks = malloc((count ? count : 1) * sizeof *s->chunks);
    s->have = calloc(count ? count : 1, 1);
    s->page_done = calloc(pages ? pages : 1, 1);
    uint8_t rnd[16];
    if (!s->chunks || !s->have || !s->page_done || plat_random_bytes(rnd, sizeof rnd) != 0) {
        free_up(s);
        cJSON_Delete(b);
        nx_reply_error(c, 500, "internal", "Out of memory.");
        return;
    }
    pthread_mutex_init(&s->mu, NULL);
    nx_hex(rnd, 16, s->id);
    s->owner = c->user.id;
    snprintf(s->name, sizeof s->name, "%s", jn->valuestring);
    s->size = size;
    memcpy(s->sha, sha, 32);
    s->chunker = chunker;
    s->count = (uint32_t)count;
    s->pages = pages;
    s->version_of = vof;
    s->last_ms = plat_now_mono_ms();
    cJSON_Delete(b);

    nx_uploads *u = app->uploads;
    pthread_mutex_lock(&u->mu);
    size_t mine = 0;
    for (up *o = u->list; o; o = o->next)
        mine += o->owner == s->owner;
    int status = 0;
    if (mine >= NX_UPLOADS_PER_USER)
        status = 429;
    else if (u->chunk_total + count > NX_UPLOAD_CHUNK_BUDGET)
        status = 503;
    else {
        s->next = u->list;
        u->list = s;
        u->count++;
        u->chunk_total += count;
    }
    pthread_mutex_unlock(&u->mu);
    if (status) {
        free_up(s);
        if (status == 429)
            nx_reply_error(c, 429, "too_many_uploads", "Finish or cancel an upload first.");
        else
            nx_reply_error(c, 503, "busy", "The server is handling many uploads. Try again shortly.");
        return;
    }
    nx_json j;
    nx_json_init(&j);
    nx_json_obj_open(&j);
    nx_json_key_str(&j, "uploadId", s->id);
    nx_json_key_u64(&j, "manifestPageSize", NX_MANIFEST_PAGE);
    nx_json_key_u64(&j, "maxChunkBody", NX_BODY_BINARY_MAX);
    nx_json_obj_close(&j);
    nx_reply_json(c, 201, &j, NULL);
    nx_json_free(&j);
    LOG_INFO("upload", "started upload=%.8s user_id=%" PRIu32 " size=%" PRIu64 " chunks=%" PRIu64,
             s->id, s->owner, size, count);
    emit_progress(app, s, "manifest", 0);
}

/* ===== POST /api/uploads/{uid}/manifest?page=N ===================================== */

void nx_h_upload_manifest(nx_ctx *c)
{
    nx_app *app = c->app;
    up *s = acquire(app->uploads, c->uid, c->user.id);
    if (!s) {
        nx_reply_error(c, 404, "upload_not_found", "No such upload (it may have expired).");
        return;
    }
    char pv[16];
    uint64_t page = UINT64_MAX;
    if (nx_query_get(c->req->query, "page", pv, sizeof pv)) {
        char *end;
        page = strtoull(pv, &end, 10);
        if (!pv[0] || *end || pv[0] == '-' || pv[0] == '+')
            page = UINT64_MAX;
    }
    if (page >= s->pages) {
        nx_reply_error(c, 400, "invalid_page", "page must be 0 .. pages - 1.");
        release(app->uploads, s);
        return;
    }
    uint32_t first_idx = (uint32_t)page * NX_MANIFEST_PAGE;
    uint32_t n = s->count - first_idx < NX_MANIFEST_PAGE ? s->count - first_idx : NX_MANIFEST_PAGE;
    if (c->body_len != (size_t)n * NX_RECIPE_CHUNK_BYTES) {
        nx_reply_error(c, 400, "invalid_manifest", "A page holds (hash[32], u32 len) for each chunk.");
        release(app->uploads, s);
        return;
    }
    nx_chunk_ref *in = malloc(n * sizeof *in);
    uint32_t *cand = malloc(n * sizeof *cand);
    if (!in || !cand) {
        free(in);
        free(cand);
        nx_reply_error(c, 500, "internal", "Out of memory.");
        release(app->uploads, s);
        return;
    }
    const char *err = NULL;
    for (uint32_t i = 0; i < n && !err; i++) {
        memcpy(in[i].hash, c->body + (size_t)i * 36, 32);
        in[i].len = nx_get_u32(c->body + (size_t)i * 36 + 32);
        if (!len_ok(s->chunker, in[i].len, first_idx + i == s->count - 1))
            err = "invalid_chunk_length";
    }
    /* One hash, one length, within the page: checked before anything in
     * the session changes, so a bad page leaves no trace. */
    nx_hmap32 page_map;
    if (!err && nx_hmap32_init(&page_map, n) != 0)
        err = "out_of_memory";
    else if (!err) {
        for (uint32_t i = 0; i < n && !err; i++) {
            bool fresh;
            uint64_t *v = nx_hmap32_upsert(&page_map, in[i].hash, &fresh);
            if (!v)
                err = "out_of_memory";
            else if (fresh)
                *v = i;
            else if (in[*v].len != in[i].len)
                err = "inconsistent_length";
        }
        nx_hmap32_free(&page_map);
    }
    size_t ncand = 0;
    pthread_mutex_lock(&s->mu);
    if (!err && s->page_done[page] &&
        memcmp(s->chunks + first_idx, in, n * sizeof *in) != 0)
        err = "page_changed"; /* a resent page must be identical */
    /* ...and against pages received earlier. */
    for (uint32_t i = 0; i < n && !err && !s->page_done[page]; i++) {
        uint64_t *v = nx_hmap32_get(&s->first, in[i].hash);
        if (v && s->chunks[*v].len != in[i].len)
            err = "inconsistent_length";
    }
    if (!err && !s->page_done[page]) {
        memcpy(s->chunks + first_idx, in, n * sizeof *in);
        for (uint32_t i = 0; i < n && !err; i++) {
            bool fresh;
            uint64_t *v = nx_hmap32_upsert(&s->first, in[i].hash, &fresh);
            if (!v)
                err = "out_of_memory"; /* the session is unusable; the client starts over */
            else if (fresh)
                *v = first_idx + i;
        }
        if (!err) {
            s->page_done[page] = 1;
            s->pages_done++;
        }
    }
    if (!err)
        for (uint32_t i = 0; i < n; i++) {
            uint64_t *v = nx_hmap32_get(&s->first, in[i].hash);
            if (v && *v == first_idx + i && !s->have[first_idx + i])
                cand[ncand++] = first_idx + i;
        }
    pthread_mutex_unlock(&s->mu);
    if (err) {
        nx_reply_error(c, strcmp(err, "page_changed") == 0 ? 409 : 400, err,
                       "The manifest page is not valid for this upload.");
        free(in);
        free(cand);
        release(app->uploads, s);
        return;
    }
    /* Ask the index which blocks the server already has (no session lock
     * held: lookups may wait for a checkpoint). */
    nx_json j;
    nx_json_init(&j);
    nx_json_obj_open(&j);
    nx_json_key(&j, "need");
    nx_json_arr_open(&j);
    uint8_t *known = calloc(ncand ? ncand : 1, 1);
    int rc = known ? 0 : -1;
    for (size_t k = 0; k < ncand && rc == 0; k++) {
        nx_index_entry ent;
        int lr = nx_engine_lookup(app->engine, s->chunks[cand[k]].hash, &ent);
        if (lr == 0 && ent.len == s->chunks[cand[k]].len)
            known[k] = 1;
        else if (lr == 0 || lr == NX_ENGINE_NOT_FOUND)
            nx_json_u64(&j, cand[k] - first_idx);
        else
            rc = -1;
    }
    nx_json_arr_close(&j);
    nx_json_obj_close(&j);
    if (rc == 0) {
        pthread_mutex_lock(&s->mu);
        for (size_t k = 0; k < ncand; k++)
            if (known[k])
                s->have[cand[k]] = 1;
        pthread_mutex_unlock(&s->mu);
        nx_reply_json(c, 200, &j, NULL);
    } else {
        nx_reply_error(c, 500, "internal", "The block index could not be read.");
    }
    nx_json_free(&j);
    free(known);
    free(in);
    free(cand);
    release(app->uploads, s);
}

/* ===== POST /api/uploads/{uid}/chunks =============================================== */

void nx_h_upload_chunks(nx_ctx *c)
{
    nx_app *app = c->app;
    up *s = acquire(app->uploads, c->uid, c->user.id);
    if (!s) {
        nx_reply_error(c, 404, "upload_not_found", "No such upload (it may have expired).");
        return;
    }
    size_t cap = c->body_len / 8 + 1;
    nx_block_in *blocks = malloc(cap * sizeof *blocks);
    uint32_t *acc = malloc(cap * sizeof *acc);
    nx_json rej;
    nx_json_init(&rej);
    nx_json_arr_open(&rej);
    if (!blocks || !acc) {
        free(blocks);
        free(acc);
        nx_json_free(&rej);
        nx_reply_error(c, 500, "internal", "Out of memory.");
        release(app->uploads, s);
        return;
    }
    size_t nacc = 0, off = 0;
    uint64_t sent = 0;
    bool malformed = false;
    while (off < c->body_len) {
        if (c->body_len - off < 8) {
            malformed = true;
            break;
        }
        uint32_t idx = nx_get_u32(c->body + off), len = nx_get_u32(c->body + off + 4);
        if (len > NX_CHUNK_MAX_SIZE || c->body_len - off - 8 < len) {
            malformed = true; /* cannot find the next record: stop here */
            break;
        }
        const uint8_t *data = c->body + off + 8;
        off += 8 + (size_t)len;
        const char *reason = NULL;
        uint8_t want[32];
        pthread_mutex_lock(&s->mu);
        if (idx >= s->count || !s->page_done[idx / NX_MANIFEST_PAGE])
            reason = "unknown_index";
        else if (s->chunks[idx].len != len)
            reason = "length_mismatch";
        else
            memcpy(want, s->chunks[idx].hash, 32);
        pthread_mutex_unlock(&s->mu);
        if (!reason) {
            uint8_t h[32];
            nx_sha256(data, len, h);
            if (memcmp(h, want, 32) != 0)
                reason = "hash_mismatch";
        }
        if (reason) {
            nx_json_obj_open(&rej);
            nx_json_key_u64(&rej, "index", idx);
            nx_json_key_str(&rej, "reason", reason);
            nx_json_obj_close(&rej);
            continue;
        }
        sent += len; /* bytes of accepted chunks: what the upload really cost */
        blocks[nacc].data = data;
        blocks[nacc].len = len;
        memcpy(blocks[nacc].hash, want, 32);
        acc[nacc++] = idx;
    }
    nx_json_arr_close(&rej);
    if (malformed) {
        nx_reply_error(c, 400, "invalid_chunks", "The body is not a sequence of (u32 index, u32 len, data).");
        goto out;
    }
    nx_store_result sr = {0};
    int rc = nacc ? nx_engine_store_blocks(app->engine, blocks, nacc, &sr) : 0;
    if (rc != 0) {
        LOG_ERROR("upload", "storing blocks failed upload=%.8s rc=%d", s->id, rc);
        nx_reply_error(c, 500, "store_failed", "The blocks could not be stored.");
        goto out;
    }
    uint64_t done;
    pthread_mutex_lock(&s->mu);
    for (size_t k = 0; k < nacc; k++) {
        uint64_t *v = nx_hmap32_get(&s->first, blocks[k].hash);
        if (v)
            s->have[*v] = 1;
        s->have[acc[k]] = 1;
    }
    s->bytes_sent += sent;
    s->new_chunks += (uint32_t)sr.stored;
    done = s->bytes_sent;
    pthread_mutex_unlock(&s->mu);
    nx_json j;
    nx_json_init(&j);
    nx_json_obj_open(&j);
    nx_json_key(&j, "accepted");
    nx_json_arr_open(&j);
    for (size_t k = 0; k < nacc; k++)
        nx_json_u64(&j, acc[k]);
    nx_json_arr_close(&j);
    nx_json_key(&j, "rejected");
    nx_json_raw(&j, rej.buf ? rej.buf : "[]", rej.buf ? rej.len : 2);
    nx_json_obj_close(&j);
    nx_reply_json(c, 200, &j, NULL);
    nx_json_free(&j);
    emit_progress(app, s, "sending", done);
out:
    nx_json_free(&rej);
    free(blocks);
    free(acc);
    release(app->uploads, s);
}

/* ===== POST /api/uploads/{uid}/commit ============================================== */

static void remove_session(nx_uploads *u, up *s)
{
    pthread_mutex_lock(&u->mu);
    if (!s->dead)
        unlink_up(u, s);
    pthread_mutex_unlock(&u->mu);
}

void nx_h_upload_commit(nx_ctx *c)
{
    nx_app *app = c->app;
    up *s = acquire(app->uploads, c->uid, c->user.id);
    if (!s) {
        nx_reply_error(c, 404, "upload_not_found", "No such upload (it may have expired).");
        return;
    }
    pthread_mutex_lock(&s->mu);
    const char *busy = s->committing ? "commit_in_progress"
                       : s->pages_done < s->pages ? "manifest_incomplete" : NULL;
    if (!busy)
        s->committing = true;
    uint64_t bytes_sent = s->bytes_sent;
    uint32_t new_chunks = s->new_chunks;
    pthread_mutex_unlock(&s->mu);
    if (busy) {
        nx_reply_error(c, 409, busy, strcmp(busy, "manifest_incomplete") == 0
                                         ? "Send every manifest page first."
                                         : "This upload is already being committed.");
        release(app->uploads, s);
        return;
    }
    emit_progress(app, s, "committing", bytes_sent);
    /* The manifest no longer changes (all pages done), so s->chunks can be
     * read without the lock. */
    nx_commit_meta m = {.owner_id = s->owner,
                        .name = s->name,
                        .chunker = s->chunker,
                        .size = s->size,
                        .bytes_sent = bytes_sent,
                        .version_of = s->version_of};
    memcpy(m.sha256, s->sha, 32);
    nx_commit_result res;
    int rc = nx_engine_commit_file(app->engine, &m, s->chunks, s->count, &res);
    if (rc == NX_ENGINE_MISSING) {
        /* Tell the client which chunk indices to send, then commit again. */
        nx_json j;
        nx_json_init(&j);
        nx_json_obj_open(&j);
        nx_json_key(&j, "error");
        nx_json_obj_open(&j);
        nx_json_key_str(&j, "code", "missing_chunks");
        nx_json_key_str(&j, "message", "Some chunks are not on the server yet.");
        nx_json_key(&j, "indices");
        nx_json_arr_open(&j);
        pthread_mutex_lock(&s->mu);
        for (size_t k = 0; k < res.missing_count; k++) {
            uint64_t *v = nx_hmap32_get(&s->first, res.missing[k]);
            if (v) {
                nx_json_u64(&j, *v);
                s->have[*v] = 0;
            }
        }
        s->committing = false;
        pthread_mutex_unlock(&s->mu);
        nx_json_arr_close(&j);
        nx_json_obj_close(&j);
        nx_json_obj_close(&j);
        nx_reply_json(c, 409, &j, NULL);
        nx_json_free(&j);
        nx_commit_result_free(&res);
        release(app->uploads, s);
        return;
    }
    if (rc != 0) {
        const char *code = rc == NX_ENGINE_BAD_HASH ? "file_hash_mismatch"
                           : rc == NX_ENGINE_INVALID ? "invalid_upload"
                           : rc == NX_ENGINE_NOT_FOUND ? "version_of_not_found"
                           : rc == NX_ENGINE_CORRUPT ? "stored_block_damaged" : "commit_failed";
        int status = rc == NX_ENGINE_BAD_HASH ? 422 : rc == NX_ENGINE_INVALID ? 400
                     : rc == NX_ENGINE_NOT_FOUND ? 404 : 500;
        nx_reply_error(c, status, code, "The file could not be committed.");
        if (rc == NX_ENGINE_BAD_HASH) {
            /* The declared file is wrong; this session cannot succeed. */
            remove_session(app->uploads, s);
        } else {
            pthread_mutex_lock(&s->mu);
            s->committing = false;
            pthread_mutex_unlock(&s->mu);
        }
        nx_commit_result_free(&res);
        release(app->uploads, s);
        return;
    }
    nx_file f;
    if (!nx_catalog_file_by_id(nx_engine_catalog(app->engine), res.file_id, &f)) {
        nx_reply_error(c, 500, "internal", "The file vanished right after commit.");
        release(app->uploads, s);
        return;
    }
    nx_json j;
    nx_json_init(&j);
    nx_json_file_open(&j, app->engine, &f);
    nx_json_key(&j, "stats");
    nx_json_obj_open(&j);
    nx_json_key_u64(&j, "chunks", s->count);
    nx_json_key_u64(&j, "newChunks", new_chunks);
    nx_json_key_u64(&j, "bytesSent", bytes_sent);
    nx_json_key(&j, "reusedPercent");
    nx_json_f64(&j, nx_reused_percent(s->size, bytes_sent));
    nx_json_obj_close(&j);
    nx_json_obj_close(&j);
    nx_reply_json(c, 201, &j, NULL);
    LOG_INFO("upload", "committed upload=%.8s file_id=%" PRIu64 " version=%" PRIu32
             " sent=%" PRIu64 " size=%" PRIu64,
             s->id, f.id, f.version, bytes_sent, s->size);
    nx_json ev;
    nx_json_init(&ev);
    nx_json_file_open(&ev, app->engine, &f);
    nx_json_key_str(&ev, "uploadId", s->id);
    nx_json_obj_close(&ev);
    emit(app, s->owner, "upload.committed", &ev);
    nx_json_free(&ev);
    nx_json_free(&j);
    remove_session(app->uploads, s);
    release(app->uploads, s);
}

/* ===== DELETE /api/uploads/{uid} ================================================== */

void nx_h_upload_cancel(nx_ctx *c)
{
    nx_app *app = c->app;
    up *s = acquire(app->uploads, c->uid, c->user.id);
    if (!s) {
        nx_reply_error(c, 404, "upload_not_found", "No such upload (it may have expired).");
        return;
    }
    nx_json j;
    nx_json_init(&j);
    nx_json_obj_open(&j);
    nx_json_key_str(&j, "uploadId", s->id);
    nx_json_key_str(&j, "fileName", s->name);
    nx_json_key_str(&j, "reason", "cancelled");
    nx_json_obj_close(&j);
    emit(app, s->owner, "upload.failed", &j);
    nx_json_free(&j);
    remove_session(app->uploads, s);
    release(app->uploads, s);
    nx_reply_empty(c, 204, NULL);
}
