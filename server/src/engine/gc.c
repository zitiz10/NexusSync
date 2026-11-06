/*
 * gc.c — cleanup and compaction (SPEC §8 "Cleanup" and "Compaction", §10;
 * DECISIONS D-048). See nx_engine_gc in engine.h.
 *
 * Cleanup frees blocks no file uses any more. A block becomes unreferenced
 * when the last file using it is deleted (FILE_DEL decrements refcounts),
 * or when an upload stored it and then never committed. Freeing only
 * removes the index entry (BLOCK_FREE); the bytes stay in their container
 * until compaction rewrites that container.
 *
 * Compaction copies the live blocks of a mostly-dead container into a
 * fresh container, logs where each block moved, and deletes the old file
 * after the next checkpoint. The order of those steps is what makes a
 * crash at any moment harmless (D-048):
 *
 *   copy + fsync  ->  commit BLOCK_MOVE.. + CONTAINER_DROP  ->  checkpoint  ->  delete file
 *        crash: copies are         crash: recovery redoes         crash: the old file has
 *        orphan bytes              the moves; old file still      nothing live; the next
 *                                  exists                         cleanup drops it
 *
 * Locks: maint_mu for the whole job (one cleanup or scan at a time); the
 * transaction mutex only while checking candidates and logging; the store
 * barrier once per compaction (SPEC §9).
 */
#include "engine/engine_internal.h"

#include "crypto/sha256.h"
#include "engine/chunk.h"
#include "util/failpoint.h"
#include "util/log.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#define FREE_BATCH 256 /* BLOCK_FREE records per transaction */

/* ===== freeing ================================================================ */

typedef struct {
    uint8_t (*h)[32];
    size_t n, cap;
    uint64_t now;
    uint32_t grace;
} hash_vec;

/* SPEC §3 gc_grace_s: "unreferenced blocks younger than this are kept". */
static bool past_grace(uint32_t created_at, uint64_t now, uint32_t grace)
{
    return (uint64_t)created_at + grace <= now;
}

static int push_hash(hash_vec *v, const uint8_t h[32])
{
    if (v->n == v->cap) {
        size_t cap = v->cap ? v->cap * 2 : 256;
        uint8_t(*nh)[32] = realloc(v->h, cap * sizeof *nh);
        if (!nh)
            return -1;
        v->h = nh;
        v->cap = cap;
    }
    memcpy(v->h[v->n++], h, 32);
    return 0;
}

static int collect_unreferenced(const nx_index_entry *ent, void *ctx)
{
    hash_vec *v = ctx;
    if (ent->refcount == 0 && past_grace(ent->created_at, v->now, v->grace))
        return push_hash(v, ent->hash);
    return 0;
}

static void reset_hashes(void *ctx) { ((hash_vec *)ctx)->n = 0; }

static int free_blocks(nx_engine *e, const nx_gc_options *o, uint64_t now, nx_gc_report *r)
{
    /* 1. Candidates from a walk under the shared directory lock: cheap, and
     *    commits keep running meanwhile. */
    hash_vec v = {.now = now, .grace = o->grace_s};
    if (nxi_walk_index(e, collect_unreferenced, &v, reset_hashes) != 0) {
        free(v.h);
        return -1;
    }
    int rc = 0;
    for (size_t i = 0; i < v.n && rc == 0; i += FREE_BATCH) {
        size_t end = v.n - i < FREE_BATCH ? v.n : i + FREE_BATCH;
        if (o->progress)
            o->progress("freeing", i, v.n, o->progress_ctx);
        nx_txn t;
        nx_txn_init(&t);
        uint64_t blocks = 0, bytes = 0;
        /* 2. Each candidate again under the transaction mutex: a commit
         *    since the walk may have started using it (SPEC §8). */
        pthread_mutex_lock(&e->txn_mu);
        for (size_t k = i; k < end && rc == 0; k++) {
            nx_index_entry ent;
            int lr = nxi_lookup(e, v.h[k], &ent, true);
            if (lr == NX_EXT_NOT_FOUND)
                continue;
            if (lr != 0) {
                rc = -1;
                break;
            }
            if (ent.refcount != 0 || !past_grace(ent.created_at, now, o->grace_s))
                continue;
            /* An open upload declared it: its commit will reference it. */
            if (o->keep && o->keep(v.h[k], o->keep_ctx))
                continue;
            rc = nx_txn_block_free(&t, v.h[k]);
            blocks++;
            bytes += ent.len;
        }
        if (rc == 0 && t.n > 0)
            rc = nxi_log_and_apply(e, &t);
        pthread_mutex_unlock(&e->txn_mu);
        nx_txn_free(&t);
        if (rc == 0) {
            r->blocks_freed += blocks;
            r->bytes_freed += bytes;
        }
    }
    if (rc == 0 && o->progress)
        o->progress("freeing", v.n, v.n, o->progress_ctx);
    free(v.h);
    return rc;
}

/* ===== compaction ============================================================== */

typedef struct {
    uint32_t id;
    uint64_t file_bytes;
    uint64_t live_bytes; /* record bytes (header + data) of indexed blocks */
    uint64_t live_count;
} candidate;

/* Live records of one container, copied into the active container. */
typedef struct {
    nx_engine *e;
    uint8_t (*hash)[32];
    nx_block_loc *from, *to;
    size_t n, cap;
    uint64_t copied; /* record bytes appended */
    bool damaged;    /* a live record failed its CRC or SHA-256 */
    bool failed;     /* I/O or memory */
} copy_ctx;

static int copy_record(const nx_block_loc *loc, const uint8_t hash[32], bool crc_ok,
                       const uint8_t *data, void *vctx)
{
    copy_ctx *x = vctx;
    nx_index_entry ent;
    int lr = nxi_lookup(x->e, hash, &ent, false);
    if (lr == NX_EXT_NOT_FOUND)
        return 0; /* freed, or never committed: dead bytes */
    if (lr != 0) {
        x->failed = true;
        return 1;
    }
    if (ent.container_id != loc->container_id || ent.offset != loc->offset)
        return 0; /* a duplicate copy; the index uses another one */
    uint8_t h[32];
    if (crc_ok && ent.len == loc->len)
        nx_sha256(data, loc->len, h);
    if (!crc_ok || ent.len != loc->len || memcmp(h, hash, 32) != 0) {
        /* Moving a damaged block would hide the evidence: keep the whole
         * container for the scan to report (D-048). */
        x->damaged = true;
        return 1;
    }
    if (x->n == x->cap) {
        size_t cap = x->cap ? x->cap * 2 : 256;
        uint8_t(*nh)[32] = realloc(x->hash, cap * sizeof *nh);
        if (nh)
            x->hash = nh;
        nx_block_loc *nf = nh ? realloc(x->from, cap * sizeof *nf) : NULL;
        if (nf)
            x->from = nf;
        nx_block_loc *nt = nf ? realloc(x->to, cap * sizeof *nt) : NULL;
        if (!nt) {
            x->failed = true;
            return 1;
        }
        x->to = nt;
        x->cap = cap;
    }
    if (nx_containers_append(x->e->ct, hash, data, loc->len, &x->to[x->n]) != 0) {
        x->failed = true;
        return 1;
    }
    memcpy(x->hash[x->n], hash, 32);
    x->from[x->n] = *loc;
    x->n++;
    x->copied += NX_RECORD_HEADER + loc->len;
    return 0;
}

/* Copies one container's live blocks and commits the moves plus its drop.
 * 0: dropped (file not deleted yet); 1: skipped, left as it is; -1: error. */
static int compact_one(nx_engine *e, const candidate *c, uint64_t *copied, nx_gc_report *r)
{
    copy_ctx x = {.e = e};
    int rc = 0;
    if (c->live_count > 0) {
        uint64_t trailing = 0;
        if (nx_containers_scan(e->ct, c->id, true, copy_record, &x, &trailing) < 0)
            x.failed = true;
        if (x.failed) {
            rc = -1;
        } else if (x.damaged || x.n != c->live_count) {
            /* Damaged, or some indexed records sit past an unparsable
             * region: the copies just made are orphan bytes. */
            LOG_WARN("gc", "container kept: a live block failed verification id=%" PRIu32
                           " live=%" PRIu64 " copied=%zu",
                     c->id, c->live_count, x.n);
            rc = 1;
        }
        /* The copies must be durable before the log says blocks moved. */
        if (rc == 0 && nx_containers_sync(e->ct) != 0)
            rc = -1;
        if (rc == 0 && nx_fail_hit("gc.compact.after_copy"))
            rc = -1;
    }
    if (rc == 0) {
        nx_txn t;
        nx_txn_init(&t);
        pthread_mutex_lock(&e->txn_mu);
        for (size_t i = 0; i < x.n && rc == 0; i++) {
            nx_index_entry ent;
            int lr = nxi_lookup(e, x.hash[i], &ent, true);
            /* Nothing else frees or moves blocks while maint_mu is held, so
             * this holds; checked anyway, because a wrong move loses data. */
            if (lr != 0 || ent.container_id != x.from[i].container_id ||
                ent.offset != x.from[i].offset) {
                LOG_WARN("gc", "block changed during compaction; container kept id=%" PRIu32, c->id);
                rc = lr == 0 || lr == NX_EXT_NOT_FOUND ? 1 : -1;
                break;
            }
            rc = nx_txn_block_move(&t, x.hash[i], x.to[i].container_id, x.to[i].offset);
        }
        if (rc == 0)
            rc = nx_txn_container_drop(&t, c->id);
        if (rc == 0)
            rc = nxi_log_and_apply(e, &t);
        pthread_mutex_unlock(&e->txn_mu);
        nx_txn_free(&t);
        if (rc == 0 && nx_fail_hit("gc.compact.after_commit"))
            rc = -1;
    }
    if (rc == 0) {
        r->blocks_moved += x.n;
        r->bytes_moved += x.copied;
        *copied = x.copied;
    }
    free(x.hash);
    free(x.from);
    free(x.to);
    return rc;
}

static int compact(nx_engine *e, const nx_gc_options *o, nx_gc_report *r)
{
    /* Containers below the active one, once every store that may still
     * log a block into them has finished (the store barrier, D-048). */
    uint32_t active = nx_containers_active_id(e->ct);
    nxi_gate_barrier(e);

    nx_block_span *sp = NULL;
    nx_container_info *ci = NULL;
    size_t ns = 0, nc = 0;
    if (nx_engine_block_spans(e, &sp, &ns) != 0 || nx_engine_containers(e, &ci, &nc) != 0) {
        free(sp);
        return -1;
    }
    candidate *cv = nc ? malloc(nc * sizeof *cv) : NULL;
    if (nc && !cv) {
        free(sp);
        free(ci);
        return -1;
    }
    size_t ncand = 0, s = 0;
    bool any_live = false;
    for (size_t k = 0; k < nc; k++) {
        candidate c = {ci[k].id, ci[k].file_bytes, 0, 0};
        while (s < ns && sp[s].container_id < c.id)
            s++;
        for (; s < ns && sp[s].container_id == c.id; s++) {
            c.live_bytes += NX_RECORD_HEADER + sp[s].len;
            c.live_count++;
        }
        if (c.id >= active)
            continue;
        uint64_t data = c.file_bytes > NX_CONTAINER_HEADER ? c.file_bytes - NX_CONTAINER_HEADER : 0;
        if (c.live_count == 0 || c.live_bytes * 2 < data) {
            cv[ncand++] = c;
            any_live = any_live || c.live_count > 0;
        }
    }
    uint64_t active_bytes = 0;
    for (size_t k = 0; k < nc; k++)
        if (ci[k].id == active)
            active_bytes = ci[k].file_bytes;
    free(sp);
    free(ci);

    int rc = 0;
    /* Moved blocks go into a fresh container (SPEC §8), so cold data that
     * survived a cleanup stays together. */
    if (any_live && active_bytes > NX_CONTAINER_HEADER && nx_containers_rotate(e->ct, NULL) != 0)
        rc = -1;

    uint32_t *dropped = ncand ? malloc(ncand * sizeof *dropped) : NULL;
    uint64_t *reclaim = ncand ? malloc(ncand * sizeof *reclaim) : NULL;
    size_t nd = 0;
    if (ncand && (!dropped || !reclaim))
        rc = -1;
    for (size_t k = 0; k < ncand && rc == 0; k++) {
        if (o->progress)
            o->progress("compacting", k, ncand, o->progress_ctx);
        uint64_t copied = 0;
        int cr = compact_one(e, &cv[k], &copied, r);
        if (cr < 0) {
            rc = -1;
        } else if (cr == 1) {
            r->containers_skipped++;
        } else {
            dropped[nd] = cv[k].id;
            reclaim[nd] = cv[k].file_bytes > copied ? cv[k].file_bytes - copied : 0;
            nd++;
        }
    }
    /* Delete the old files only after a checkpoint: until it writes the
     * moved locations, the index pages on disk still point into them. */
    if (rc == 0 && nd > 0) {
        if (nx_engine_checkpoint(e) != 0)
            rc = -1;
        if (rc == 0 && nx_fail_hit("gc.compact.after_checkpoint"))
            rc = -1;
        for (size_t k = 0; k < nd && rc == 0; k++) {
            if (nx_fail_hit("gc.compact.before_delete")) {
                rc = -1;
                break;
            }
            if (nx_containers_drop(e->ct, dropped[k]) != 0) {
                /* Harmless: nothing in it is live; the next cleanup retries. */
                LOG_WARN("gc", "could not delete compacted container id=%" PRIu32 " err=\"%s\"",
                         dropped[k], plat_last_error());
                continue;
            }
            r->containers_compacted++;
            r->bytes_reclaimed += reclaim[k];
        }
    }
    if (rc == 0 && o->progress)
        o->progress("compacting", ncand, ncand, o->progress_ctx);
    free(dropped);
    free(reclaim);
    free(cv);
    return rc;
}

/* ===== entry point ============================================================== */

int nx_engine_gc(nx_engine *e, const nx_gc_options *o, nx_gc_report *r)
{
    memset(r, 0, sizeof *r);
    uint64_t t0 = plat_now_mono_ms();
    uint64_t now = o->now_s ? o->now_s : plat_now_unix_s();
    pthread_mutex_lock(&e->maint_mu);
    int rc = free_blocks(e, o, now, r);
    if (rc == 0)
        rc = compact(e, o, r);
    pthread_mutex_unlock(&e->maint_mu);
    r->ms = plat_now_mono_ms() - t0;
    if (rc == 0)
        LOG_INFO("gc",
                 "cleanup done blocks_freed=%" PRIu64 " bytes_freed=%" PRIu64
                 " containers_compacted=%" PRIu64 " containers_skipped=%" PRIu64
                 " blocks_moved=%" PRIu64 " bytes_reclaimed=%" PRIu64 " ms=%" PRIu64,
                 r->blocks_freed, r->bytes_freed, r->containers_compacted, r->containers_skipped,
                 r->blocks_moved, r->bytes_reclaimed, r->ms);
    else if (!nx_fail_crashed())
        LOG_ERROR("gc", "cleanup failed");
    return rc;
}
