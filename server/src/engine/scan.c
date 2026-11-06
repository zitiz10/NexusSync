/*
 * scan.c — the integrity scan, finding the files a block belongs to, and
 * --rebuild-index (SPEC §10, DECISIONS D-048).
 *
 * The scan answers "is every stored block still the bytes we were given?"
 * by reading each indexed block and checking three things:
 *   1. the record's CRC-32 (catches bit rot and torn writes cheaply),
 *   2. the hash stored in the record matches the index key (catches an
 *      index entry that points at the wrong record),
 *   3. SHA-256 of the data equals that hash (catches damage the CRC
 *      happened to miss).
 * Downloads already do 2 and 3 for the blocks they read (verify_on_read);
 * the scan also covers blocks nobody has downloaded lately.
 *
 * --rebuild-index answers "the index is lost; can we get it back?" Yes,
 * because every container record carries its block's hash (SPEC §5) and
 * every file's recipe lists its blocks: the index is derived data.
 */
#include "engine/engine_internal.h"

#include "crypto/sha256.h"
#include "engine/chunk.h"
#include "util/hmap.h"
#include "util/log.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ===== integrity scan ============================================================ */

typedef struct {
    nx_index_entry *v;
    size_t n, cap;
} entry_vec;

static int collect_entry(const nx_index_entry *ent, void *ctx)
{
    entry_vec *ev = ctx;
    if (ev->n == ev->cap) {
        size_t cap = ev->cap ? ev->cap * 2 : 1024;
        nx_index_entry *nv = realloc(ev->v, cap * sizeof *nv);
        if (!nv)
            return -1;
        ev->v = nv;
        ev->cap = cap;
    }
    ev->v[ev->n++] = *ent;
    return 0;
}

static void reset_entries(void *ctx) { ((entry_vec *)ctx)->n = 0; }

/* Storage order, so the scan reads each container front to back. */
static int entry_cmp(const void *pa, const void *pb)
{
    const nx_index_entry *a = pa, *b = pb;
    if (a->container_id != b->container_id)
        return a->container_id < b->container_id ? -1 : 1;
    return a->offset < b->offset ? -1 : a->offset > b->offset;
}

static uint8_t check_block(nx_engine *e, const nx_index_entry *ent, uint8_t *buf)
{
    nx_block_loc loc = {ent->container_id, ent->offset, ent->len};
    uint8_t stored[32];
    bool corrupt = false;
    if (ent->len == 0 || ent->len > NX_CHUNK_MAX_SIZE)
        return NX_SCAN_CRC;
    if (nx_containers_read(e->ct, &loc, buf, NX_CHUNK_MAX_SIZE, stored, &corrupt) != 0)
        return corrupt ? NX_SCAN_CRC : NX_SCAN_UNREADABLE;
    if (memcmp(stored, ent->hash, 32) != 0)
        return NX_SCAN_HASH;
    uint8_t h[32];
    nx_sha256(buf, ent->len, h);
    return memcmp(h, ent->hash, 32) == 0 ? 0 : NX_SCAN_HASH;
}

int nx_engine_scan(nx_engine *e, nx_progress_cb progress, void *ctx, nx_scan_report *r)
{
    memset(r, 0, sizeof *r);
    uint64_t t0 = plat_now_mono_ms();
    uint8_t *buf = malloc(NX_CHUNK_MAX_SIZE);
    if (!buf)
        return -1;
    /* maint_mu: cleanup cannot free or move a block while we read it. */
    pthread_mutex_lock(&e->maint_mu);
    entry_vec ev = {0};
    int rc = nxi_walk_index(e, collect_entry, &ev, reset_entries);
    if (rc == 0 && ev.n > 1)
        qsort(ev.v, ev.n, sizeof *ev.v, entry_cmp);
    if (rc == 0 && progress)
        progress("scanning", 0, ev.n, ctx);
    uint64_t last_pct = 0;
    for (size_t i = 0; rc == 0 && i < ev.n; i++) {
        const nx_index_entry *ent = &ev.v[i];
        uint8_t why = check_block(e, ent, buf);
        r->checked++;
        if (why == 0) {
            r->verified++;
        } else {
            r->failures++;
            char hx[17];
            nx_hex(ent->hash, 8, hx);
            LOG_ERROR("scan", "damaged block=%s container=%" PRIu32 " offset=%" PRIu32 " reason=%s", hx,
                      ent->container_id, ent->offset,
                      why == NX_SCAN_CRC ? "crc" : why == NX_SCAN_HASH ? "hash" : "unreadable");
            if (r->n_items < NX_SCAN_MAX_ITEMS) {
                nx_scan_failure *f = &r->items[r->n_items++];
                memcpy(f->hash, ent->hash, 32);
                f->container_id = ent->container_id;
                f->offset = ent->offset;
                f->reason = why;
            }
        }
        uint64_t pct = (uint64_t)(i + 1) * 100u / ev.n;
        if (progress && pct != last_pct) {
            progress("scanning", i + 1, ev.n, ctx);
            last_pct = pct;
        }
    }
    pthread_mutex_unlock(&e->maint_mu);
    free(ev.v);
    free(buf);
    r->ms = plat_now_mono_ms() - t0;
    if (rc == 0)
        LOG_INFO("scan", "integrity scan done checked=%" PRIu64 " failures=%" PRIu64 " ms=%" PRIu64,
                 r->checked, r->failures, r->ms);
    return rc;
}

/* ===== which files use these blocks ================================================ */

int nx_engine_files_using(nx_engine *e, const uint8_t (*hashes)[32], size_t n, nx_file_use_cb cb,
                          void *ctx)
{
    if (n == 0)
        return 0;
    nx_hmap32 want;
    if (nx_hmap32_init(&want, n) != 0)
        return -1;
    bool *hit = calloc(n, sizeof *hit);
    int rc = hit ? 0 : -1;
    for (size_t i = 0; i < n && rc == 0; i++) {
        uint64_t *v = nx_hmap32_upsert(&want, hashes[i], NULL);
        if (!v)
            rc = -1;
        else
            *v = i;
    }
    nx_file *fv = NULL;
    size_t nf = 0;
    if (rc == 0)
        rc = nx_catalog_files(e->cat, NULL, NULL, &fv, &nf);
    for (size_t f = 0; f < nf && rc == 0; f++) {
        nx_chunk_ref *chunks;
        int fr = nx_engine_file_recipe(e, fv[f].id, NULL, &chunks);
        if (fr != 0) {
            if (fr == -1)
                rc = -1;
            continue; /* an unreadable recipe: fsck reports it */
        }
        memset(hit, 0, n * sizeof *hit);
        for (uint32_t k = 0; k < fv[f].chunk_count && rc == 0; k++) {
            uint64_t *v = nx_hmap32_get(&want, chunks[k].hash);
            if (v && !hit[*v]) {
                hit[*v] = true;
                if (cb((size_t)*v, &fv[f], ctx) != 0)
                    rc = -1;
            }
        }
        free(chunks);
    }
    free(fv);
    free(hit);
    nx_hmap32_free(&want);
    return rc;
}

/* ===== --rebuild-index ================================================================ */

typedef struct {
    nx_engine *e;
    nx_rebuild_report *r;
    uint32_t now;
    bool failed;
} rebuild_ctx;

static int rebuild_record(const nx_block_loc *loc, const uint8_t hash[32], bool crc_ok,
                          const uint8_t *data, void *vctx)
{
    rebuild_ctx *x = vctx;
    x->r->records++;
    uint8_t h[32];
    if (!crc_ok || loc->len == 0 || loc->len > NX_CHUNK_MAX_SIZE) {
        x->r->damaged++;
        return 0;
    }
    nx_sha256(data, loc->len, h);
    if (memcmp(h, hash, 32) != 0) {
        x->r->damaged++;
        return 0;
    }
    nx_index_entry ent;
    memcpy(ent.hash, hash, 32);
    ent.container_id = loc->container_id;
    ent.offset = loc->offset;
    ent.len = loc->len;
    ent.refcount = 0;     /* counted from the recipes afterwards */
    ent.created_at = x->now; /* unknown: a fresh grace period */
    int rc = nxi_index_insert(x->e, &ent);
    if (rc == 0) {
        x->r->entries++;
    } else if (rc == NX_EXT_EXISTS) {
        x->r->duplicates++; /* the first copy in storage order wins */
    } else {
        x->failed = true;
        return 1;
    }
    return 0;
}

/* Moves index/<name> to index/<name>.old if it exists. */
static int move_aside(const char *data_dir, const char *name)
{
    char src[1100], dst[1100];
    snprintf(src, sizeof src, "%s/index/%s", data_dir, name);
    snprintf(dst, sizeof dst, "%s/index/%s.old", data_dir, name);
    plat_file *f;
    if (plat_file_open(src, PLAT_O_READ, &f) != 0)
        return 0; /* already gone */
    plat_file_close(f);
    if (plat_rename_replace(src, dst) != 0) {
        LOG_ERROR("rebuild", "cannot move the old index aside path=%s err=\"%s\"", src,
                  plat_last_error());
        return -1;
    }
    return 0;
}

/* Reference counts from every catalog file (deleted-flagged ones too,
 * D-017). Fills refs: hash -> number of uses. */
static int count_references(nx_engine *e, nx_hmap32 *refs, nx_rebuild_report *r)
{
    nx_file *fv = NULL;
    size_t nf = 0;
    if (nx_catalog_files(e->cat, NULL, NULL, &fv, &nf) != 0)
        return -1;
    int rc = 0;
    for (size_t f = 0; f < nf && rc == 0; f++) {
        nx_chunk_ref *chunks;
        int fr = nx_engine_file_recipe(e, fv[f].id, NULL, &chunks);
        if (fr != 0) {
            if (fr == -1) {
                rc = -1;
                break;
            }
            LOG_ERROR("rebuild", "recipe unreadable file_id=%" PRIu64, fv[f].id);
            r->files_affected++;
            continue;
        }
        r->files++;
        for (uint32_t k = 0; k < fv[f].chunk_count && rc == 0; k++) {
            uint64_t *v = nx_hmap32_upsert(refs, chunks[k].hash, NULL);
            if (!v)
                rc = -1;
            else
                (*v)++;
        }
        free(chunks);
    }
    free(fv);
    return rc;
}

/* Counts the files that use any hash in `missing`. */
static int count_affected(nx_engine *e, nx_hmap32 *missing, nx_rebuild_report *r)
{
    nx_file *fv = NULL;
    size_t nf = 0;
    if (nx_catalog_files(e->cat, NULL, NULL, &fv, &nf) != 0)
        return -1;
    for (size_t f = 0; f < nf; f++) {
        nx_chunk_ref *chunks;
        if (nx_engine_file_recipe(e, fv[f].id, NULL, &chunks) != 0)
            continue; /* already counted by count_references */
        for (uint32_t k = 0; k < fv[f].chunk_count; k++)
            if (nx_hmap32_get(missing, chunks[k].hash)) {
                r->files_affected++;
                LOG_ERROR("rebuild", "file uses a block found in no container file_id=%" PRIu64
                                     " name=\"%s\"",
                          fv[f].id, fv[f].name);
                break;
            }
        free(chunks);
    }
    free(fv);
    return 0;
}

int nx_engine_rebuild_index(const nx_engine_config *cfg, nx_rebuild_report *r)
{
    memset(r, 0, sizeof *r);
    if (!cfg->data_dir || strlen(cfg->data_dir) > 900)
        return -1;
    snprintf(r->old_index, sizeof r->old_index, "%s/index/*.old", cfg->data_dir);
    char idx[1100];
    snprintf(idx, sizeof idx, "%s/index", cfg->data_dir);
    if (move_aside(cfg->data_dir, "dir.bin") != 0 || move_aside(cfg->data_dir, "buckets.bin") != 0 ||
        plat_fsync_dir(idx) != 0)
        return -1;

    /* Opens with an empty index; replay applies only catalog records. */
    nx_engine *e;
    if (nxi_open(cfg, true, &e) != 0)
        return -1;
    int rc = 0;
    nx_hmap32 refs, missing;
    bool have_refs = nx_hmap32_init(&refs, 1024) == 0;
    bool have_missing = nx_hmap32_init(&missing, 16) == 0;
    if (!have_refs || !have_missing)
        rc = -1;

    pthread_mutex_lock(&e->txn_mu);
    /* 1. Every verified record, containers in id order. */
    uint32_t *ids = NULL;
    size_t nids = 0;
    if (rc == 0)
        rc = nx_containers_list(e->ct, &ids, &nids);
    rebuild_ctx x = {e, r, (uint32_t)plat_now_unix_s(), false};
    for (size_t i = 0; i < nids && rc == 0; i++) {
        uint64_t trailing = 0;
        if (nx_containers_scan(e->ct, ids[i], true, rebuild_record, &x, &trailing) < 0 || x.failed)
            rc = -1;
        r->containers++;
        if (trailing)
            LOG_WARN("rebuild", "unparsable tail skipped container=%" PRIu32 " bytes=%" PRIu64, ids[i],
                     trailing);
    }
    free(ids);
    /* 2. Reference counts from the recipes. */
    if (rc == 0)
        rc = count_references(e, &refs, r);
    for (size_t i = 0; rc == 0 && i < refs.cap; i++) {
        if (refs.used[i] != 1)
            continue;
        const uint8_t *h = refs.keys + 32 * i;
        uint64_t uses = refs.vals[i];
        int ar = nxi_index_add_ref(e, h, uses > UINT32_MAX ? UINT32_MAX : (uint32_t)uses);
        if (ar == NX_EXT_NOT_FOUND) {
            r->missing_blocks++;
            if (!nx_hmap32_upsert(&missing, h, NULL))
                rc = -1;
        } else if (ar != 0) {
            rc = -1;
        }
    }
    if (rc == 0 && r->missing_blocks > 0)
        rc = count_affected(e, &missing, r);
    /* 3. Make the new index durable. */
    if (rc == 0)
        rc = nxi_checkpoint_locked(e);
    pthread_mutex_unlock(&e->txn_mu);
    if (rc == 0)
        rc = nxi_recount_blocks(e);
    if (have_refs)
        nx_hmap32_free(&refs);
    if (have_missing)
        nx_hmap32_free(&missing);
    if (nx_engine_close(e) != 0)
        rc = -1;
    if (rc != 0)
        return -1;
    LOG_INFO("rebuild",
             "index rebuilt containers=%" PRIu64 " records=%" PRIu64 " entries=%" PRIu64
             " duplicates=%" PRIu64 " damaged=%" PRIu64 " files=%" PRIu64 " missing_blocks=%" PRIu64,
             r->containers, r->records, r->entries, r->duplicates, r->damaged, r->files,
             r->missing_blocks);
    return r->missing_blocks || r->files_affected ? 1 : 0;
}
