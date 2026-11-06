/*
 * engine.c — ties containers, index, catalog, and recipes together into
 * transactions (SPEC §7.5, §8). See include/engine.h.
 *
 * Locking (SPEC §9, DECISIONS D-016):
 *   txn_mu   one transaction applies at a time: assigning LSNs, (M5) the WAL
 *            append and fsync, applying records to pages, and checkpoints.
 *            Applies therefore happen in LSN order, which page-LSN redo
 *            needs (D-013).
 *   catalog  rwlock inside catalog.c
 *   index    directory rwlock: shared for lookups and record applies,
 *            exclusive to split a bucket
 *   frames, container append mutex, recipe mutex: leaves inside modules.
 * Readers (lookups, downloads) never take txn_mu except to run a
 * checkpoint when the buffer pool is full, and then hold nothing else.
 */
#include "engine.h"

#include "crypto/sha256.h"
#include "engine/checkpoint.h"
#include "engine/chunk.h"
#include "engine/container.h"
#include "engine/logrec.h"
#include "engine/superblock.h"
#include "engine/wal.h"
#include "platform/platform.h"
#include "util/failpoint.h"
#include "util/hmap.h"
#include "util/log.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Page files in the buffer pool (and in the doublewrite file). */
enum { FID_BUCKETS = 0, FID_USERS = 1, FID_FILES = 2, FID_DIR = 3, FID_COUNT = 4 };

#define MAX_RETRIES 10000 /* pool-full / split retries before giving up */

struct nx_engine {
    nx_engine_config cfg;
    char dir[1024];
    char paths[FID_COUNT][1100];
    char dw_path[1100];

    nx_superblock sb;
    plat_file *files[FID_COUNT];
    nx_bufpool *bp;
    nx_exthash *ix;
    nx_catalog *cat;
    nx_recipes *rec;
    nx_containers *ct;
    nx_wal *wal;

    pthread_mutex_t txn_mu;
    /* Under txn_mu: */
    uint64_t next_lsn;   /* next LSN to hand out */
    uint64_t stable_lsn; /* every record <= this is fully applied */
    bool broken;         /* a write failed after the commit point, or the log failed */
    bool recovering;     /* replaying the log: checkpoints keep the log intact */
    nx_engine_recovery rinfo;

    _Atomic uint64_t next_file_id;
    _Atomic uint64_t last_lsn; /* copy of stable_lsn for readers of stats */
    _Atomic uint64_t logical, physical, unique_blocks, saved;
    _Atomic uint64_t checkpoints, last_ckpt_pages, last_ckpt_ms, ckpt_lsn;
    _Atomic uint64_t last_ckpt_at; /* unix ms of the last checkpoint this run (0: none yet) */
};

void nx_engine_config_defaults(nx_engine_config *cfg)
{
    cfg->data_dir = "./data";
    cfg->container_size = 4194304;
    cfg->pool_pages = 4096;
    cfg->verify_on_read = true;
    cfg->verify_on_commit = true;
    cfg->wal_segment_size = NX_WAL_DEFAULT_SEGMENT;
    cfg->replay_progress = NULL;
}

static void hex8(const uint8_t hash[32], char out[17])
{
    nx_hex(hash, 8, out); /* first 8 bytes are enough to identify a block in logs */
}

/* ===== data directory ====================================================== */

static const char *const SUBDIRS[] = {"containers", "index", "catalog", "recipes", "wal"};

int nx_engine_create(const char *data_dir)
{
    nx_superblock sb;
    int rc = nx_superblock_load(data_dir, &sb);
    if (rc != 1) {
        LOG_ERROR("engine", "data dir already initialized (or damaged) dir=%s", data_dir);
        return -1;
    }
    if (plat_mkdir_p(data_dir) != 0)
        return -1;
    char path[1100];
    for (size_t i = 0; i < sizeof SUBDIRS / sizeof SUBDIRS[0]; i++) {
        snprintf(path, sizeof path, "%s/%s", data_dir, SUBDIRS[i]);
        if (plat_mkdir_p(path) != 0)
            return -1;
    }
    if (plat_fsync_dir(data_dir) != 0)
        return -1;
    nx_superblock_init(&sb, plat_now_unix_s());
    if (nx_superblock_write(data_dir, &sb) != 0)
        return -1;
    LOG_INFO("engine", "initialized dir=%s", data_dir);
    return 0;
}

/* Copies a complete doublewrite batch in place (recovery step 2). */
static int repair_from_doublewrite(nx_engine *e)
{
    e->rinfo.pages_repaired = 0;
    nx_dw_page *pages;
    uint8_t *store;
    size_t n;
    uint64_t lsn;
    if (nx_dw_load(e->dw_path, &pages, &store, &n, &lsn) != 0)
        return -1;
    if (n == 0)
        return nx_dw_clear(e->dw_path);
    plat_file *f[FID_COUNT] = {0};
    int rc = 0;
    for (size_t i = 0; i < n && rc == 0; i++) {
        uint32_t fid = pages[i].file_id;
        if (fid >= FID_COUNT) {
            LOG_ERROR("engine", "doublewrite names unknown file id=%" PRIu32, fid);
            rc = -1;
            break;
        }
        if (!f[fid] && plat_file_open(e->paths[fid], PLAT_O_WRITE | PLAT_O_CREATE, &f[fid]) != 0) {
            rc = -1;
            break;
        }
        rc = plat_pwrite_all(f[fid], pages[i].data, NX_PAGE_SIZE,
                             (uint64_t)pages[i].page_no * NX_PAGE_SIZE);
    }
    for (int i = 0; i < FID_COUNT; i++)
        if (f[i]) {
            if (rc == 0 && plat_fsync_file(f[i]) != 0)
                rc = -1;
            if (plat_file_close(f[i]) != 0)
                rc = -1;
        }
    free(pages);
    free(store);
    if (rc == 0)
        rc = nx_dw_clear(e->dw_path);
    if (rc == 0) {
        e->rinfo.pages_repaired = n;
        LOG_INFO("engine", "repaired pages from doublewrite pages=%zu checkpoint_lsn=%" PRIu64, n,
                 lsn);
    }
    return rc;
}

/* ===== checkpoint ============================================================ */

typedef struct {
    nx_dw_page *pages;
    uint8_t *store;
    size_t n, cap;
} page_batch;

static int batch_add(page_batch *b, uint32_t fid, uint32_t page_no, const uint8_t *data)
{
    if (b->n == b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 64;
        nx_dw_page *p = realloc(b->pages, cap * sizeof *p);
        if (!p)
            return -1;
        b->pages = p;
        uint8_t *s = realloc(b->store, cap * NX_PAGE_SIZE);
        if (!s)
            return -1;
        b->store = s;
        b->cap = cap;
    }
    memcpy(b->store + b->n * NX_PAGE_SIZE, data, NX_PAGE_SIZE);
    b->pages[b->n].file_id = fid;
    b->pages[b->n].page_no = page_no;
    b->n++;
    return 0;
}

static int collect_dirty(uint32_t fid, uint32_t page_no, const uint8_t *page, void *ctx)
{
    return batch_add(ctx, fid, page_no, page);
}

/*
 * SPEC §8 checkpoint, with txn_mu held. lsn is the checkpoint LSN: every
 * change up to it is in the pages being written. While a transaction is
 * half applied, callers pass the LSN just before its BEGIN (stable_lsn), so
 * recovery replays the whole transaction and page LSNs skip what is
 * already there (D-013).
 */
static int checkpoint_locked(nx_engine *e, uint64_t lsn)
{
    uint64_t t0 = plat_now_mono_ms();
    page_batch b = {0};
    int rc = nx_bufpool_for_each_dirty(e->bp, collect_dirty, &b);
    bool dir_dirty = nx_exthash_dir_dirty(e->ix, false);
    if (rc == 0 && dir_dirty) {
        uint8_t *img = NULL;
        size_t len = 0;
        nx_exthash_lock_shared(e->ix);
        rc = nx_exthash_dir_image(e->ix, &img, &len);
        nx_exthash_unlock(e->ix);
        for (size_t off = 0; rc == 0 && off < len; off += NX_PAGE_SIZE)
            rc = batch_add(&b, FID_DIR, (uint32_t)(off / NX_PAGE_SIZE), img + off);
        free(img);
    }
    /* Pointers into store are fixed only once collection has finished. */
    for (size_t i = 0; i < b.n; i++)
        b.pages[i].data = b.store + i * NX_PAGE_SIZE;

    nx_exthash_stats xs;
    nx_exthash_get_stats(e->ix, &xs);
    uint64_t next_file = atomic_load(&e->next_file_id);
    uint32_t active = nx_containers_active_id(e->ct);
    bool sb_same = e->sb.checkpoint_lsn == lsn && e->sb.next_file_id == next_file &&
                   e->sb.active_container_id == active && e->sb.index_global_depth == xs.global_depth &&
                   e->sb.bucket_page_count == xs.buckets;
    if (rc != 0 || (b.n == 0 && sb_same))
        goto out;

    if (nx_fail_hit("ckpt.begin")) {
        rc = -1;
        goto out;
    }
    if (b.n > 0) {
        /* 1. Stage every page in the doublewrite file and fsync it. */
        rc = nx_dw_write(e->dw_path, b.pages, b.n, lsn);
        if (rc == 0 && nx_fail_hit("ckpt.after_doublewrite"))
            rc = -1;
        /* 2. Write each page in place, then fsync each file. */
        for (size_t i = 0; rc == 0 && i < b.n; i++) {
            const nx_dw_page *p = &b.pages[i];
            bool already = nx_fail_crashed();
            if (nx_fail_hit("ckpt.page.torn")) {
                /* Crash in the middle of overwriting a page: a torn page,
                 * which only the doublewrite copy can repair. */
                if (!already)
                    plat_pwrite_all(e->files[p->file_id], p->data, NX_PAGE_SIZE / 2,
                                    (uint64_t)p->page_no * NX_PAGE_SIZE);
                rc = -1;
                break;
            }
            if (p->file_id == FID_DIR)
                rc = plat_pwrite_all(e->files[FID_DIR], p->data, NX_PAGE_SIZE,
                                     (uint64_t)p->page_no * NX_PAGE_SIZE);
            else
                rc = nx_bufpool_write_page(e->bp, p->file_id, p->page_no, p->data);
        }
        if (rc == 0)
            rc = nx_bufpool_sync_files(e->bp);
        if (rc == 0 && dir_dirty)
            rc = plat_fsync_file(e->files[FID_DIR]);
    }
    if (rc == 0 && nx_fail_hit("ckpt.before_superblock"))
        rc = -1;
    /* 3. New superblock copy pointing at this checkpoint. */
    if (rc == 0) {
        nx_superblock sb = e->sb;
        sb.checkpoint_lsn = lsn;
        sb.next_file_id = next_file;
        sb.next_container_id = active + 1;
        sb.active_container_id = active;
        sb.index_global_depth = xs.global_depth;
        sb.bucket_page_count = xs.buckets;
        rc = nx_superblock_write(e->dir, &sb);
        if (rc == 0)
            e->sb = sb;
    }
    if (rc == 0 && nx_fail_hit("ckpt.before_clear"))
        rc = -1;
    /* 4. Empty the doublewrite file; the pages are now clean. */
    if (rc == 0 && b.n > 0) {
        rc = nx_dw_clear(e->dw_path);
        if (rc == 0) {
            nx_bufpool_mark_all_clean(e->bp);
            nx_exthash_dir_dirty(e->ix, true);
        }
    }
    /* 5. The log before the checkpoint is no longer needed. While
     * recovering, the log is being read, so it is left alone. */
    if (rc == 0 && !e->recovering) {
        if (nx_wal_truncate_below(e->wal, lsn) != 0)
            LOG_WARN("engine", "could not delete old log segments lsn=%" PRIu64, lsn);
        uint8_t pl[8];
        for (int k = 0; k < 8; k++)
            pl[k] = (uint8_t)(lsn >> (8 * k));
        nx_wal_rec cr = {e->next_lsn, 0, NX_REC_CHECKPOINT, 8, pl};
        /* Not fsynced: it only marks a safe end of the log; the next
         * commit's fsync makes it durable. */
        if (nx_wal_append(e->wal, &cr, 1, false) != 0) {
            e->broken = true;
            rc = -1;
        } else {
            e->next_lsn++;
        }
    }
    if (rc == 0) {
        uint64_t ms = plat_now_mono_ms() - t0;
        atomic_fetch_add(&e->checkpoints, 1);
        atomic_store(&e->last_ckpt_pages, b.n);
        atomic_store(&e->last_ckpt_ms, ms);
        atomic_store(&e->ckpt_lsn, lsn);
        atomic_store(&e->last_ckpt_at, plat_now_unix_ms());
        LOG_DEBUG("engine", "checkpoint lsn=%" PRIu64 " pages=%zu ms=%" PRIu64, lsn, b.n, ms);
    }
out:
    if (rc != 0)
        LOG_ERROR("engine", "checkpoint failed lsn=%" PRIu64, lsn);
    free(b.pages);
    free(b.store);
    return rc;
}

int nx_engine_checkpoint(nx_engine *e)
{
    pthread_mutex_lock(&e->txn_mu);
    int rc = checkpoint_locked(e, e->stable_lsn);
    pthread_mutex_unlock(&e->txn_mu);
    return rc;
}

/* A reader hit NX_BP_FULL: it holds no lock now; run a checkpoint so dirty
 * frames become evictable, then it retries. */
static int reader_checkpoint(nx_engine *e, bool have_txn)
{
    if (have_txn)
        return checkpoint_locked(e, e->stable_lsn);
    return nx_engine_checkpoint(e);
}

/* ===== index access with split / pool-full retries ============================ */

enum ix_op { IX_LOOKUP, IX_INSERT, IX_ADD_REF, IX_SET_LOC, IX_REMOVE };

typedef struct {
    enum ix_op op;
    const uint8_t *hash;
    nx_index_entry *entry; /* in for insert, out for lookup */
    int64_t delta;
    uint32_t container, offset;
    uint64_t lsn;
    bool redo;
    uint32_t new_ref;
} ix_call;

/* have_txn: the caller holds txn_mu (appliers, fsck). Appliers checkpoint
 * at stable_lsn, which is the LSN before the transaction being applied. */
static int index_call(nx_engine *e, ix_call *c, bool have_txn)
{
    for (int attempt = 0; attempt < MAX_RETRIES; attempt++) {
        nx_exthash_lock_shared(e->ix);
        int rc;
        switch (c->op) {
        case IX_LOOKUP: rc = nx_exthash_lookup(e->ix, c->hash, c->entry); break;
        case IX_INSERT: rc = nx_exthash_insert(e->ix, c->entry, c->lsn, c->redo); break;
        case IX_ADD_REF:
            rc = nx_exthash_add_ref(e->ix, c->hash, c->delta, c->lsn, c->redo, &c->new_ref);
            break;
        case IX_SET_LOC:
            rc = nx_exthash_set_location(e->ix, c->hash, c->container, c->offset, c->lsn, c->redo);
            break;
        case IX_REMOVE: rc = nx_exthash_remove(e->ix, c->hash, c->lsn, c->redo); break;
        default: rc = -1;
        }
        nx_exthash_unlock(e->ix);
        if (rc == NX_EXT_NEED_SPLIT) {
            nx_exthash_lock_exclusive(e->ix);
            rc = nx_exthash_split(e->ix, c->hash);
            nx_exthash_unlock(e->ix);
            if (rc == NX_BP_FULL && reader_checkpoint(e, have_txn) != 0)
                return -1;
            if (rc != 0 && rc != NX_BP_FULL)
                return -1;
            continue;
        }
        if (rc == NX_BP_FULL) {
            if (reader_checkpoint(e, have_txn) != 0)
                return -1;
            continue;
        }
        return rc;
    }
    LOG_ERROR("engine", "index operation kept failing op=%d", (int)c->op);
    return -1;
}

static int lookup(nx_engine *e, const uint8_t hash[32], nx_index_entry *out, bool have_txn)
{
    ix_call c = {.op = IX_LOOKUP, .hash = hash, .entry = out};
    return index_call(e, &c, have_txn);
}

int nx_engine_lookup(nx_engine *e, const uint8_t hash[32], nx_index_entry *out)
{
    int rc = lookup(e, hash, out, false);
    return rc == NX_EXT_NOT_FOUND ? NX_ENGINE_NOT_FOUND : rc;
}

/* ===== apply: the one place records change pages ================================= */

/* Catalog changes, retried after a checkpoint when the pool is full. */
enum cat_op { CAT_ADD_FILE, CAT_SET_FLAGS, CAT_DEL_FILE, CAT_PUT_USER };

static int catalog_call(nx_engine *e, enum cat_op op, const nx_file *f, const nx_user *u,
                        uint64_t id, uint8_t flags, uint64_t lsn)
{
    for (int attempt = 0; attempt < MAX_RETRIES; attempt++) {
        int rc;
        switch (op) {
        case CAT_ADD_FILE: rc = nx_catalog_add_file(e->cat, f, lsn); break;
        case CAT_SET_FLAGS: rc = nx_catalog_set_file_flags(e->cat, id, flags, lsn); break;
        case CAT_DEL_FILE: rc = nx_catalog_delete_file(e->cat, id, lsn); break;
        case CAT_PUT_USER: rc = nx_catalog_put_user(e->cat, u, lsn); break;
        default: rc = -1;
        }
        if (rc != NX_BP_FULL)
            return rc;
        if (checkpoint_locked(e, e->stable_lsn) != 0)
            return -1;
    }
    return -1;
}

static int apply_record(nx_engine *e, uint8_t type, const uint8_t *p, uint32_t len, uint64_t lsn,
                        bool redo)
{
    char hx[17];
    switch (type) {
    case NX_REC_BEGIN:
    case NX_REC_COMMIT:
    case NX_REC_CHECKPOINT:
    case NX_REC_CONTAINER_DROP: /* the file is removed after the next checkpoint (M12) */
        return 0;

    case NX_REC_CHUNK_ADD: {
        nx_index_entry ent;
        if (nx_rec_decode_chunk_add(p, len, &ent) != 0)
            return -1;
        ix_call c = {.op = IX_INSERT, .hash = ent.hash, .entry = &ent, .lsn = lsn, .redo = redo};
        int rc = index_call(e, &c, true);
        if (rc == 0) {
            atomic_fetch_add(&e->physical, ent.len);
            atomic_fetch_add(&e->unique_blocks, 1);
        }
        return (rc == 0 || rc == NX_EXT_EXISTS || rc == NX_EXT_SKIPPED) ? 0 : -1;
    }

    case NX_REC_REF_INC:
    case NX_REC_REF_DEC: {
        uint8_t hash[32];
        uint32_t count;
        if (nx_rec_decode_ref(p, len, hash, &count) != 0)
            return -1;
        ix_call c = {.op = IX_ADD_REF,
                     .hash = hash,
                     .delta = type == NX_REC_REF_INC ? (int64_t)count : -(int64_t)count,
                     .lsn = lsn,
                     .redo = redo};
        int rc = index_call(e, &c, true);
        if (rc == 0 || rc == NX_EXT_SKIPPED)
            return 0;
        hex8(hash, hx);
        LOG_ERROR("engine", "refcount change failed type=%s block=%s rc=%d", nx_rec_type_name(type),
                  hx, rc);
        return -1;
    }

    case NX_REC_FILE_ADD: {
        nx_file f;
        if (nx_file_decode(p, len, &f) != 0)
            return -1;
        int rc = catalog_call(e, CAT_ADD_FILE, &f, NULL, 0, 0, lsn);
        if (rc == 0) {
            atomic_fetch_add(&e->logical, f.size);
            atomic_fetch_add(&e->saved, f.size > f.bytes_sent ? f.size - f.bytes_sent : 0);
        }
        return rc == 0 || rc == 1 ? 0 : -1;
    }

    case NX_REC_FILE_UPDATE: {
        uint64_t id;
        uint8_t flags;
        if (nx_rec_decode_file_update(p, len, &id, &flags) != 0)
            return -1;
        int rc = catalog_call(e, CAT_SET_FLAGS, NULL, NULL, id, flags, lsn);
        return rc == 0 || rc == 1 ? 0 : -1;
    }

    case NX_REC_FILE_DEL: {
        uint64_t id;
        if (nx_rec_decode_u64(p, len, &id) != 0)
            return -1;
        nx_file f;
        bool had = nx_catalog_file_by_id(e->cat, id, &f);
        int rc = catalog_call(e, CAT_DEL_FILE, NULL, NULL, id, 0, lsn);
        if (rc == 0 && had) {
            atomic_fetch_sub(&e->logical, f.size);
            atomic_fetch_sub(&e->saved, f.size > f.bytes_sent ? f.size - f.bytes_sent : 0);
        }
        return rc == 0 || rc == 1 ? 0 : -1;
    }

    case NX_REC_USER_ADD:
    case NX_REC_USER_UPDATE: {
        nx_user u;
        if (nx_user_decode(p, len, &u) != 0)
            return -1;
        int rc = catalog_call(e, CAT_PUT_USER, NULL, &u, 0, 0, lsn);
        return rc == 0 ? 0 : -1;
    }

    case NX_REC_BLOCK_FREE: {
        if (len != NX_BLOCK_FREE_LEN)
            return -1;
        nx_index_entry ent;
        bool known = lookup(e, p, &ent, true) == 0;
        ix_call c = {.op = IX_REMOVE, .hash = p, .lsn = lsn, .redo = redo};
        int rc = index_call(e, &c, true);
        if (rc == 0 && known) {
            atomic_fetch_sub(&e->physical, ent.len);
            atomic_fetch_sub(&e->unique_blocks, 1);
        }
        return (rc == 0 || rc == NX_EXT_NOT_FOUND || rc == NX_EXT_SKIPPED) ? 0 : -1;
    }

    case NX_REC_BLOCK_MOVE: {
        uint8_t hash[32];
        uint32_t cid, off;
        if (nx_rec_decode_block_move(p, len, hash, &cid, &off) != 0)
            return -1;
        ix_call c = {.op = IX_SET_LOC, .hash = hash, .container = cid, .offset = off, .lsn = lsn,
                     .redo = redo};
        int rc = index_call(e, &c, true);
        return (rc == 0 || rc == NX_EXT_SKIPPED) ? 0 : -1;
    }

    default:
        LOG_ERROR("engine", "unknown record type=%u", (unsigned)type);
        return -1;
    }
}

/*
 * Logs and applies one transaction. Caller holds txn_mu.
 *
 * M4: LSNs are handed out in memory. M5 inserts the WAL append of BEGIN,
 * the records, and COMMIT, plus the WAL fsync, at the marked point: that
 * fsync is the commit point, and only after it are pages changed
 * (redo-only logging with no-steal, SPEC §8).
 */
static int log_and_apply(nx_engine *e, const nx_txn *t)
{
    if (e->broken)
        return -1;
    /* Keep dirty pages under half the pool (SPEC §7.3). */
    if (nx_bufpool_dirty_count(e->bp) * 2 > nx_bufpool_frame_count(e->bp) &&
        checkpoint_locked(e, e->stable_lsn) != 0)
        return -1;

    nx_wal_rec *recs = malloc((t->n + 2) * sizeof *recs);
    if (!recs)
        return -1;
    uint64_t begin = e->next_lsn;
    uint64_t commit = begin + 1 + t->n;
    recs[0] = (nx_wal_rec){begin, begin, NX_REC_BEGIN, 0, NULL};
    for (size_t i = 0; i < t->n; i++)
        recs[i + 1] = (nx_wal_rec){begin + 1 + i, begin, t->recs[i].type, t->recs[i].len,
                                   nx_txn_payload(t, i)};
    recs[t->n + 1] = (nx_wal_rec){commit, begin, NX_REC_COMMIT, 0, NULL};
    /* The commit point: BEGIN..COMMIT written and fsynced. Only after this
     * may pages change (redo-only logging with no-steal, SPEC §8). */
    int rc = nx_wal_append(e->wal, recs, t->n + 2, true);
    free(recs);
    if (rc != 0) {
        e->broken = true; /* the log refuses further writes until restart */
        return -1;
    }
    e->next_lsn = commit + 1;
    if (nx_fail_hit("txn.after_commit")) {
        e->broken = true;
        return -1;
    }

    for (size_t i = 0; i < t->n; i++) {
        const nx_txn_rec *r = &t->recs[i];
        if ((i == 1 && nx_fail_hit("txn.mid_apply")) ||
            apply_record(e, r->type, nx_txn_payload(t, i), r->len, begin + 1 + i, false) != 0) {
            /* The transaction is committed but not fully applied: the pages
             * no longer match the log. Stop accepting writes; a restart
             * replays the transaction from the WAL. */
            e->broken = true;
            if (!nx_fail_crashed())
                LOG_ERROR("engine", "apply failed, engine is read-only until restart lsn=%" PRIu64,
                          begin + 1 + i);
            return -1;
        }
    }
    e->stable_lsn = commit;
    atomic_store(&e->last_lsn, commit);
    return 0;
}

/* ===== open / close ============================================================== */

static int count_entry(const nx_index_entry *ent, void *ctx)
{
    nx_engine *e = ctx;
    atomic_fetch_add(&e->physical, ent->len);
    atomic_fetch_add(&e->unique_blocks, 1);
    return 0;
}

static int init_stats(nx_engine *e)
{
    nx_file *fv;
    size_t n;
    if (nx_catalog_files(e->cat, NULL, NULL, &fv, &n) != 0)
        return -1;
    for (size_t i = 0; i < n; i++) {
        atomic_fetch_add(&e->logical, fv[i].size);
        atomic_fetch_add(&e->saved, fv[i].size > fv[i].bytes_sent ? fv[i].size - fv[i].bytes_sent : 0);
    }
    free(fv);
    nx_exthash_lock_shared(e->ix);
    int rc = nx_exthash_for_each(e->ix, count_entry, e);
    nx_exthash_unlock(e->ix);
    return rc;
}

static int read_all(plat_file *f, uint8_t **buf, size_t *len)
{
    uint64_t size;
    *buf = NULL;
    *len = 0;
    if (plat_file_size(f, &size) != 0 || size > (64u << 20))
        return -1;
    if (size == 0)
        return 0;
    *buf = malloc((size_t)size);
    if (!*buf)
        return -1;
    if (plat_pread(f, *buf, (size_t)size, 0) != (int64_t)size) {
        free(*buf);
        *buf = NULL;
        return -1;
    }
    *len = (size_t)size;
    return 0;
}

static int page_count_of(plat_file *f, const char *path, uint32_t *pages)
{
    uint64_t size;
    if (plat_file_size(f, &size) != 0)
        return -1;
    /* Pages are only ever written whole, at checkpoints, after a doublewrite;
     * a partial page means the file was damaged outside the engine. */
    if (size % NX_PAGE_SIZE != 0 || size / NX_PAGE_SIZE > UINT32_MAX) {
        LOG_ERROR("engine", "page file has a partial page path=%s size=%" PRIu64, path, size);
        return -1;
    }
    *pages = (uint32_t)(size / NX_PAGE_SIZE);
    return 0;
}

static void free_engine(nx_engine *e)
{
    nx_wal_close(e->wal);
    nx_containers_close(e->ct);
    nx_recipes_close(e->rec);
    nx_catalog_close(e->cat);
    nx_exthash_close(e->ix);
    nx_bufpool_destroy(e->bp);
    for (int i = 0; i < FID_COUNT; i++)
        if (e->files[i] && plat_file_close(e->files[i]) != 0)
            LOG_ERROR("engine", "close failed path=%s", e->paths[i]);
    pthread_mutex_destroy(&e->txn_mu);
    free(e);
}

/* ---- recovery: replay committed transactions from the log (SPEC §8) ---- */

typedef struct {
    nx_engine *e;
    nx_txn pending; /* records of the transaction being read */
    uint64_t begin; /* its BEGIN lsn (= txn id) */
    bool open;
    uint64_t max_file_id;
    uint32_t max_user_id;
} replay_ctx;

static int replay_record(const nx_wal_rec *r, void *vctx)
{
    replay_ctx *x = vctx;
    nx_engine *e = x->e;
    switch (r->type) {
    case NX_REC_BEGIN:
        if (x->open) /* cannot happen with one writer; count it, drop it */
            e->rinfo.txns_discarded++;
        nx_txn_free(&x->pending);
        nx_txn_init(&x->pending);
        x->open = true;
        x->begin = r->lsn;
        return 0;
    case NX_REC_CHECKPOINT:
        return 0;
    case NX_REC_COMMIT:
        if (!x->open || r->txn_id != x->begin) {
            LOG_WARN("engine", "COMMIT without its BEGIN ignored lsn=%" PRIu64, r->lsn);
            return 0;
        }
        /* Redo every record of the committed transaction, in LSN order,
         * through the normal apply code (redo=true skips what is there). */
        for (size_t i = 0; i < x->pending.n; i++) {
            const nx_txn_rec *tr = &x->pending.recs[i];
            const uint8_t *p = nx_txn_payload(&x->pending, i);
            if (apply_record(e, tr->type, p, tr->len, x->begin + 1 + i, true) != 0) {
                LOG_ERROR("engine", "redo failed lsn=%" PRIu64 " type=%s", x->begin + 1 + i,
                          nx_rec_type_name(tr->type));
                return -1;
            }
            nx_file f;
            nx_user u;
            if (tr->type == NX_REC_FILE_ADD && nx_file_decode(p, tr->len, &f) == 0 &&
                f.id > x->max_file_id)
                x->max_file_id = f.id;
            if ((tr->type == NX_REC_USER_ADD || tr->type == NX_REC_USER_UPDATE) &&
                nx_user_decode(p, tr->len, &u) == 0 && u.id > x->max_user_id)
                x->max_user_id = u.id;
        }
        e->rinfo.records_replayed += x->pending.n;
        e->rinfo.txns_redone++;
        if (e->cfg.replay_progress)
            atomic_store(e->cfg.replay_progress, e->rinfo.records_replayed);
        e->stable_lsn = r->lsn; /* a checkpoint now may start after this txn */
        nx_txn_free(&x->pending);
        x->open = false;
        return 0;
    default:
        if (!x->open || r->txn_id != x->begin) {
            LOG_WARN("engine", "record outside a transaction ignored lsn=%" PRIu64, r->lsn);
            return 0;
        }
        return nx_txn_add(&x->pending, r->type, r->payload, r->len);
    }
}

static int replay_log(nx_engine *e, replay_ctx *x)
{
    memset(x, 0, sizeof *x);
    x->e = e;
    nx_txn_init(&x->pending);
    e->stable_lsn = e->sb.checkpoint_lsn;
    e->recovering = true;
    nx_wal_recovery wr;
    int rc = nx_wal_recover(e->wal, e->sb.checkpoint_lsn, replay_record, x, &wr);
    e->recovering = false;
    if (x->open)
        e->rinfo.txns_discarded++; /* never committed; the log cut it */
    nx_txn_free(&x->pending);
    if (rc != 0)
        return -1;
    e->rinfo.wal_bytes_cut = wr.bytes_cut;
    e->next_lsn = nx_wal_next_lsn(e->wal);
    /* Everything kept in the log is committed and now applied. */
    e->stable_lsn = e->next_lsn - 1;
    return 0;
}

int nx_engine_open(const nx_engine_config *cfg, nx_engine **out)
{
    if (!cfg->data_dir || strlen(cfg->data_dir) > 900 || cfg->pool_pages < 16)
        return -1;
    nx_engine *e = calloc(1, sizeof *e);
    if (!e)
        return -1;
    e->cfg = *cfg;
    snprintf(e->dir, sizeof e->dir, "%s", cfg->data_dir);
    e->cfg.data_dir = e->dir;
    snprintf(e->paths[FID_BUCKETS], sizeof e->paths[0], "%s/index/buckets.bin", e->dir);
    snprintf(e->paths[FID_DIR], sizeof e->paths[0], "%s/index/dir.bin", e->dir);
    snprintf(e->paths[FID_USERS], sizeof e->paths[0], "%s/catalog/users.tbl", e->dir);
    snprintf(e->paths[FID_FILES], sizeof e->paths[0], "%s/catalog/files.tbl", e->dir);
    snprintf(e->dw_path, sizeof e->dw_path, "%s/doublewrite.bin", e->dir);
    pthread_mutex_init(&e->txn_mu, NULL);

    char path[1100];
    int rc = nx_superblock_load(e->dir, &e->sb);
    if (rc != 0) {
        if (rc == 1)
            LOG_ERROR("engine", "data dir not initialized (run --init) dir=%s", e->dir);
        goto fail;
    }
    if (!nx_superblock_params_match(&e->sb)) {
        LOG_ERROR("engine", "data dir uses different chunker parameters; refusing to open");
        goto fail;
    }
    if (repair_from_doublewrite(e) != 0)
        goto fail;

    for (int i = 0; i < FID_COUNT; i++)
        if (plat_file_open(e->paths[i], PLAT_O_WRITE | PLAT_O_CREATE, &e->files[i]) != 0) {
            LOG_ERROR("engine", "cannot open path=%s err=%s", e->paths[i], plat_last_error());
            goto fail;
        }
    if (nx_bufpool_create(cfg->pool_pages, &e->bp) != 0 ||
        nx_bufpool_attach(e->bp, FID_BUCKETS, e->files[FID_BUCKETS]) != 0 ||
        nx_bufpool_attach(e->bp, FID_USERS, e->files[FID_USERS]) != 0 ||
        nx_bufpool_attach(e->bp, FID_FILES, e->files[FID_FILES]) != 0)
        goto fail;

    uint8_t *dir_img;
    size_t dir_len;
    uint32_t bucket_pages, user_pages, file_pages;
    if (read_all(e->files[FID_DIR], &dir_img, &dir_len) != 0 ||
        page_count_of(e->files[FID_BUCKETS], e->paths[FID_BUCKETS], &bucket_pages) != 0)
        goto fail;
    if (dir_len == 0 && bucket_pages > 0) {
        LOG_ERROR("engine", "bucket pages exist but dir.bin is empty (try --rebuild-index)");
        goto fail;
    }
    rc = nx_exthash_open(e->bp, FID_BUCKETS, dir_img, dir_len, &e->ix);
    free(dir_img);
    if (rc != 0)
        goto fail;
    if (page_count_of(e->files[FID_USERS], e->paths[FID_USERS], &user_pages) != 0 ||
        page_count_of(e->files[FID_FILES], e->paths[FID_FILES], &file_pages) != 0 ||
        nx_catalog_open(e->bp, FID_USERS, user_pages, FID_FILES, file_pages, &e->cat) != 0)
        goto fail;
    snprintf(path, sizeof path, "%s/recipes/recipes.dat", e->dir);
    if (nx_recipes_open(path, &e->rec) != 0)
        goto fail;
    snprintf(path, sizeof path, "%s/containers", e->dir);
    if (nx_containers_open(path, cfg->container_size, &e->ct) != 0)
        goto fail;

    uint64_t t0 = plat_now_mono_ms();
    e->rinfo.checkpoint_lsn = e->sb.checkpoint_lsn;
    snprintf(path, sizeof path, "%s/wal", e->dir);
    if (nx_wal_open(path, cfg->wal_segment_size, e->sb.checkpoint_lsn + 1, &e->wal) != 0)
        goto fail;
    replay_ctx rx;
    if (replay_log(e, &rx) != 0)
        goto fail;

    /* Next ids (SPEC §8 recovery): never reuse an id that is in the catalog
     * or that a replayed record used (it may have been deleted since). */
    uint64_t nf = e->sb.next_file_id, cmax = nx_catalog_max_file_id(e->cat) + 1;
    if (nf < cmax)
        nf = cmax;
    if (nf < rx.max_file_id + 1)
        nf = rx.max_file_id + 1;
    atomic_store(&e->next_file_id, nf);
    uint32_t nu = nx_catalog_max_user_id(e->cat) + 1;
    if (nu < rx.max_user_id + 1)
        nu = rx.max_user_id + 1;
    if (e->sb.next_user_id < nu)
        e->sb.next_user_id = nu;
    atomic_store(&e->ckpt_lsn, e->sb.checkpoint_lsn);
    atomic_store(&e->last_lsn, e->stable_lsn);
    /* Redo may have counted changes the pages already had: recount. */
    atomic_store(&e->logical, 0);
    atomic_store(&e->physical, 0);
    atomic_store(&e->unique_blocks, 0);
    atomic_store(&e->saved, 0);
    if (init_stats(e) != 0)
        goto fail;
    /* Recovery step 5: checkpoint, so the replayed work is in the pages and
     * the old log can go. Also makes a brand-new index durable. */
    if (checkpoint_locked(e, e->stable_lsn) != 0)
        goto fail;
    e->rinfo.ms = plat_now_mono_ms() - t0;
    LOG_INFO("engine",
             "recovery records_replayed=%" PRIu64 " txns_redone=%" PRIu64 " txns_discarded=%" PRIu64
             " wal_bytes_cut=%" PRIu64 " pages_repaired=%" PRIu64 " ms=%" PRIu64,
             e->rinfo.records_replayed, e->rinfo.txns_redone, e->rinfo.txns_discarded,
             e->rinfo.wal_bytes_cut, e->rinfo.pages_repaired, e->rinfo.ms);
    LOG_INFO("engine", "opened dir=%s files=%zu blocks=%" PRIu64 " checkpoint_lsn=%" PRIu64, e->dir,
             nx_catalog_file_count(e->cat), atomic_load(&e->unique_blocks), e->sb.checkpoint_lsn);
    *out = e;
    return 0;
fail:
    free_engine(e);
    return -1;
}

int nx_engine_close(nx_engine *e)
{
    if (!e)
        return 0;
    pthread_mutex_lock(&e->txn_mu);
    int rc = e->broken ? -1 : checkpoint_locked(e, e->stable_lsn);
    pthread_mutex_unlock(&e->txn_mu);
    free_engine(e);
    return rc;
}

void nx_engine_abandon(nx_engine *e)
{
    if (e)
        free_engine(e); /* closes handles; writes nothing */
}

void nx_engine_last_recovery(nx_engine *e, nx_engine_recovery *out) { *out = e->rinfo; }

nx_catalog *nx_engine_catalog(nx_engine *e) { return e->cat; }

/* ===== store blocks ================================================================ */

int nx_engine_store_blocks(nx_engine *e, const nx_block_in *blocks, size_t n, nx_store_result *res)
{
    memset(res, 0, sizeof *res);
    if (n == 0)
        return 0;
    /* Never trust the client: check sizes and hash every block first. */
    for (size_t i = 0; i < n; i++) {
        if (!blocks[i].data || blocks[i].len == 0 || blocks[i].len > NX_CHUNK_MAX_SIZE)
            return NX_ENGINE_INVALID;
        uint8_t h[32];
        nx_sha256(blocks[i].data, blocks[i].len, h);
        if (memcmp(h, blocks[i].hash, 32) != 0)
            return NX_ENGINE_BAD_HASH;
    }
    nx_hmap32 seen;
    if (nx_hmap32_init(&seen, n) != 0)
        return -1;
    nx_txn t;
    nx_txn_init(&t);
    int rc = 0;
    uint32_t now = (uint32_t)plat_now_unix_s();
    size_t appended = 0;
    for (size_t i = 0; i < n && rc == 0; i++) {
        bool fresh;
        if (!nx_hmap32_upsert(&seen, blocks[i].hash, &fresh)) {
            rc = -1;
            break;
        }
        if (!fresh) {
            res->duplicate++;
            continue;
        }
        nx_index_entry ent;
        int lr = lookup(e, blocks[i].hash, &ent, false);
        if (lr == 0) {
            res->duplicate++;
            continue;
        }
        if (lr != NX_EXT_NOT_FOUND) {
            rc = -1;
            break;
        }
        nx_block_loc loc;
        if (nx_containers_append(e->ct, blocks[i].hash, blocks[i].data, blocks[i].len, &loc) != 0) {
            rc = -1;
            break;
        }
        memcpy(ent.hash, blocks[i].hash, 32);
        ent.container_id = loc.container_id;
        ent.offset = loc.offset;
        ent.len = loc.len;
        ent.refcount = 0;
        ent.created_at = now;
        if (nx_txn_chunk_add(&t, &ent) != 0)
            rc = -1;
        appended++;
    }
    /* The blocks must be durable before the log says they exist. */
    if (rc == 0 && appended > 0)
        rc = nx_containers_sync(e->ct);
    if (rc == 0 && t.n > 0) {
        pthread_mutex_lock(&e->txn_mu);
        if (log_and_apply(e, &t) != 0) {
            rc = -1;
        } else {
            /* Count what each CHUNK_ADD really did: another upload may have
             * stored the same block a moment earlier (SPEC §9). Its copy in
             * our container is orphan bytes that compaction reclaims. */
            for (size_t i = 0; i < t.n; i++) {
                nx_index_entry ent;
                nx_rec_decode_chunk_add(nx_txn_payload(&t, i), t.recs[i].len, &ent);
                nx_index_entry cur;
                if (lookup(e, ent.hash, &cur, true) == 0 && cur.container_id == ent.container_id &&
                    cur.offset == ent.offset) {
                    res->stored++;
                    res->bytes_written += ent.len;
                } else {
                    res->duplicate++;
                }
            }
        }
        pthread_mutex_unlock(&e->txn_mu);
    }
    nx_txn_free(&t);
    nx_hmap32_free(&seen);
    return rc;
}

/* ===== commit / delete ================================================================ */

void nx_commit_result_free(nx_commit_result *r)
{
    free(r->missing);
    r->missing = NULL;
    r->missing_count = 0;
}

/* Distinct hashes in order of first appearance, with their counts. */
typedef struct {
    nx_hmap32 map; /* hash -> index into order */
    uint32_t *order; /* chunk index of the first appearance */
    uint32_t *count;
    size_t n;
} distinct_set;

static int distinct_build(distinct_set *d, const nx_chunk_ref *chunks, uint32_t count)
{
    memset(d, 0, sizeof *d);
    if (nx_hmap32_init(&d->map, count) != 0)
        return -1;
    d->order = malloc((count ? count : 1) * sizeof *d->order);
    d->count = malloc((count ? count : 1) * sizeof *d->count);
    if (!d->order || !d->count)
        return -1;
    for (uint32_t i = 0; i < count; i++) {
        bool fresh;
        uint64_t *v = nx_hmap32_upsert(&d->map, chunks[i].hash, &fresh);
        if (!v)
            return -1;
        if (fresh) {
            *v = d->n;
            d->order[d->n] = i;
            d->count[d->n] = 0;
            d->n++;
        }
        d->count[*v]++;
    }
    return 0;
}

static void distinct_free(distinct_set *d)
{
    nx_hmap32_free(&d->map);
    free(d->order);
    free(d->count);
}

/* Reads one block of a recipe and checks it against its fingerprint. */
static int read_block(nx_engine *e, const nx_chunk_ref *c, uint8_t *buf, bool verify, bool have_txn)
{
    nx_index_entry ent;
    int rc = lookup(e, c->hash, &ent, have_txn);
    if (rc == NX_EXT_NOT_FOUND)
        return NX_ENGINE_CORRUPT;
    if (rc != 0)
        return -1;
    if (ent.len != c->len)
        return NX_ENGINE_CORRUPT;
    nx_block_loc loc = {ent.container_id, ent.offset, ent.len};
    uint8_t stored_hash[32];
    bool corrupt = false;
    if (nx_containers_read(e->ct, &loc, buf, NX_CHUNK_MAX_SIZE, stored_hash, &corrupt) != 0)
        return corrupt ? NX_ENGINE_CORRUPT : -1;
    /* The inline hash catches an index entry that points at the wrong
     * record; re-hashing catches damaged bytes the CRC missed. */
    if (memcmp(stored_hash, c->hash, 32) != 0)
        return NX_ENGINE_CORRUPT;
    if (verify) {
        uint8_t h[32];
        nx_sha256(buf, c->len, h);
        if (memcmp(h, c->hash, 32) != 0)
            return NX_ENGINE_CORRUPT;
    }
    return 0;
}

static int check_chunks_shape(const nx_chunk_ref *chunks, uint32_t count, uint64_t size)
{
    uint64_t total = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (chunks[i].len == 0 || chunks[i].len > NX_CHUNK_MAX_SIZE)
            return -1;
        total += chunks[i].len;
    }
    return total == size ? 0 : -1;
}

/* Collects distinct hashes that are not in the index. */
static int find_missing(nx_engine *e, const nx_chunk_ref *chunks, const distinct_set *d,
                        bool have_txn, nx_commit_result *res)
{
    for (size_t k = 0; k < d->n; k++) {
        const nx_chunk_ref *c = &chunks[d->order[k]];
        nx_index_entry ent;
        int rc = lookup(e, c->hash, &ent, have_txn);
        if (rc == 0) {
            if (ent.len != c->len)
                return NX_ENGINE_INVALID; /* a known hash with a different length */
            continue;
        }
        if (rc != NX_EXT_NOT_FOUND)
            return -1;
        if (!res->missing) {
            res->missing = malloc(d->n * sizeof *res->missing);
            if (!res->missing)
                return -1;
        }
        memcpy(res->missing[res->missing_count++], c->hash, 32);
    }
    return res->missing_count ? NX_ENGINE_MISSING : 0;
}

static int verify_whole_file(nx_engine *e, const nx_chunk_ref *chunks, uint32_t count,
                             const uint8_t want[32])
{
    uint8_t *buf = malloc(NX_CHUNK_MAX_SIZE);
    if (!buf)
        return -1;
    nx_sha256_ctx sc;
    nx_sha256_init(&sc);
    int rc = 0;
    for (uint32_t i = 0; i < count && rc == 0; i++) {
        rc = read_block(e, &chunks[i], buf, true, false);
        if (rc == 0)
            nx_sha256_update(&sc, buf, chunks[i].len);
    }
    free(buf);
    if (rc != 0)
        return rc;
    uint8_t got[32];
    nx_sha256_final(&sc, got);
    return memcmp(got, want, 32) == 0 ? 0 : NX_ENGINE_BAD_HASH;
}

int nx_engine_commit_file(nx_engine *e, const nx_commit_meta *m, const nx_chunk_ref *chunks,
                          uint32_t count, nx_commit_result *res)
{
    memset(res, 0, sizeof *res);
    size_t name_len = m->name ? strlen(m->name) : 0;
    if (name_len == 0 || name_len > NX_FILENAME_MAX ||
        (m->chunker != NX_CHUNKER_FIXED && m->chunker != NX_CHUNKER_CDC) ||
        (count > 0 && !chunks) || check_chunks_shape(chunks, count, m->size) != 0 ||
        nx_recipe_record_len(count) == 0)
        return NX_ENGINE_INVALID;

    distinct_set d;
    int rc = distinct_build(&d, chunks, count);
    /* Early check without the lock: most missing-chunk errors are found
     * here, before anything is written. */
    if (rc == 0)
        rc = find_missing(e, chunks, &d, false, res);
    if (rc == 0 && e->cfg.verify_on_commit)
        rc = verify_whole_file(e, chunks, count, m->sha256);
    uint64_t file_id = 0, rec_off = 0;
    uint32_t rec_len = 0;
    if (rc == 0) {
        file_id = atomic_fetch_add(&e->next_file_id, 1);
        /* The recipe is durable before the transaction that points at it. */
        rc = nx_recipes_append(e->rec, file_id, chunks, count, &rec_off, &rec_len);
    }
    if (rc != 0) {
        distinct_free(&d);
        return rc;
    }

    nx_txn t;
    nx_txn_init(&t);
    pthread_mutex_lock(&e->txn_mu);
    /* Re-check under txn_mu: cleanup (also under txn_mu) may have freed an
     * unreferenced block since the early check (SPEC §8 "commit file"). */
    rc = find_missing(e, chunks, &d, true, res);
    nx_file f;
    if (rc == 0) {
        memset(&f, 0, sizeof f);
        f.id = file_id;
        f.owner_id = m->owner_id;
        memcpy(f.name, m->name, name_len + 1);
        nx_file prev;
        bool has_prev = false;
        if (m->version_of == 0)
            has_prev = nx_catalog_latest_version(e->cat, m->owner_id, m->name, &prev);
        else if (m->version_of != NX_VERSION_NONE) {
            has_prev = nx_catalog_file_by_id(e->cat, m->version_of, &prev) &&
                       prev.owner_id == m->owner_id;
            if (!has_prev)
                rc = NX_ENGINE_NOT_FOUND;
        }
        if (has_prev) {
            f.version = prev.version + 1;
            f.prev_version_id = prev.id;
        } else {
            f.version = 1;
        }
        f.size = m->size;
        f.chunk_count = count;
        f.chunker = m->chunker;
        f.recipe_offset = rec_off;
        f.recipe_len = rec_len;
        memcpy(f.sha256, m->sha256, 32);
        f.bytes_sent = m->bytes_sent;
        f.created_at = plat_now_unix_ms();
        for (size_t k = 0; k < d.n && rc == 0; k++)
            rc = nx_txn_ref(&t, NX_REC_REF_INC, chunks[d.order[k]].hash, d.count[k]);
        if (rc == 0)
            rc = nx_txn_file_add(&t, &f);
        if (rc == 0)
            rc = log_and_apply(e, &t);
    }
    pthread_mutex_unlock(&e->txn_mu);
    nx_txn_free(&t);
    distinct_free(&d);
    if (rc == 0) {
        res->file_id = file_id;
        res->version = f.version;
    }
    return rc;
}

int nx_engine_file_recipe(nx_engine *e, uint64_t file_id, nx_file *meta, nx_chunk_ref **chunks)
{
    nx_file f;
    if (!nx_catalog_file_by_id(e->cat, file_id, &f))
        return NX_ENGINE_NOT_FOUND;
    int rc = nx_recipes_read(e->rec, f.recipe_offset, f.recipe_len, f.id, f.chunk_count, chunks);
    if (rc == NX_RECIPE_CORRUPT)
        return NX_ENGINE_CORRUPT;
    if (rc != 0)
        return -1;
    if (check_chunks_shape(*chunks, f.chunk_count, f.size) != 0) {
        free(*chunks);
        *chunks = NULL;
        return NX_ENGINE_CORRUPT;
    }
    if (meta)
        *meta = f;
    return 0;
}

int nx_engine_delete_file(nx_engine *e, uint64_t file_id)
{
    nx_file f;
    nx_chunk_ref *chunks;
    int rc = nx_engine_file_recipe(e, file_id, &f, &chunks);
    if (rc != 0)
        return rc;
    distinct_set d;
    nx_txn t;
    nx_txn_init(&t);
    rc = distinct_build(&d, chunks, f.chunk_count);
    pthread_mutex_lock(&e->txn_mu);
    /* Two deletes of one file race: only the first finds it still there. */
    if (rc == 0 && !nx_catalog_file_by_id(e->cat, file_id, NULL))
        rc = NX_ENGINE_NOT_FOUND;
    if (rc == 0)
        rc = nx_txn_file_del(&t, file_id);
    for (size_t k = 0; k < d.n && rc == 0; k++)
        rc = nx_txn_ref(&t, NX_REC_REF_DEC, chunks[d.order[k]].hash, d.count[k]);
    if (rc == 0)
        rc = log_and_apply(e, &t);
    pthread_mutex_unlock(&e->txn_mu);
    nx_txn_free(&t);
    distinct_free(&d);
    free(chunks);
    return rc;
}

int nx_engine_set_file_flags(nx_engine *e, uint64_t file_id, uint8_t flags)
{
    nx_txn t;
    nx_txn_init(&t);
    pthread_mutex_lock(&e->txn_mu);
    int rc = nx_catalog_file_by_id(e->cat, file_id, NULL) ? 0 : NX_ENGINE_NOT_FOUND;
    if (rc == 0)
        rc = nx_txn_file_update(&t, file_id, flags);
    if (rc == 0)
        rc = log_and_apply(e, &t);
    pthread_mutex_unlock(&e->txn_mu);
    nx_txn_free(&t);
    return rc;
}

/* ===== read ============================================================================= */

static int read_file_impl(nx_engine *e, uint64_t file_id, nx_engine_write_cb cb, void *ctx,
                          bool verify, bool have_txn)
{
    nx_file f;
    nx_chunk_ref *chunks;
    int rc = nx_engine_file_recipe(e, file_id, &f, &chunks);
    if (rc != 0)
        return rc;
    uint8_t *buf = malloc(NX_CHUNK_MAX_SIZE);
    if (!buf) {
        free(chunks);
        return -1;
    }
    nx_sha256_ctx sc;
    nx_sha256_init(&sc);
    for (uint32_t i = 0; i < f.chunk_count && rc == 0; i++) {
        rc = read_block(e, &chunks[i], buf, verify, have_txn);
        if (rc == NX_ENGINE_CORRUPT) {
            char hx[17];
            hex8(chunks[i].hash, hx);
            LOG_ERROR("engine", "corrupt block file_id=%" PRIu64 " chunk=%" PRIu32 " block=%s",
                      file_id, i, hx);
        }
        if (rc != 0)
            break;
        nx_sha256_update(&sc, buf, chunks[i].len);
        if (cb && cb(buf, chunks[i].len, ctx) != 0)
            rc = NX_ENGINE_STOPPED;
    }
    if (rc == 0) {
        uint8_t got[32];
        nx_sha256_final(&sc, got);
        if (memcmp(got, f.sha256, 32) != 0) {
            LOG_ERROR("engine", "whole-file hash mismatch file_id=%" PRIu64, file_id);
            rc = NX_ENGINE_CORRUPT;
        }
    }
    free(buf);
    free(chunks);
    return rc;
}

int nx_engine_read_file(nx_engine *e, uint64_t file_id, nx_engine_write_cb cb, void *ctx)
{
    return read_file_impl(e, file_id, cb, ctx, e->cfg.verify_on_read, false);
}

/* ===== users ================================================================================ */

int nx_engine_add_user(nx_engine *e, nx_user *u)
{
    if (strlen(u->username) == 0 || strlen(u->username) > NX_USERNAME_MAX ||
        strlen(u->display) > NX_DISPLAY_MAX)
        return NX_ENGINE_INVALID;
    nx_txn t;
    nx_txn_init(&t);
    pthread_mutex_lock(&e->txn_mu);
    int rc = 0;
    if (nx_catalog_user_by_name(e->cat, u->username, NULL)) {
        rc = NX_ENGINE_EXISTS;
    } else {
        u->id = e->sb.next_user_id++;
        rc = nx_txn_user(&t, NX_REC_USER_ADD, u);
        if (rc == 0)
            rc = log_and_apply(e, &t);
    }
    pthread_mutex_unlock(&e->txn_mu);
    nx_txn_free(&t);
    return rc;
}

int nx_engine_update_user(nx_engine *e, const nx_user *u)
{
    if (strlen(u->username) == 0 || strlen(u->username) > NX_USERNAME_MAX ||
        strlen(u->display) > NX_DISPLAY_MAX)
        return NX_ENGINE_INVALID;
    nx_txn t;
    nx_txn_init(&t);
    pthread_mutex_lock(&e->txn_mu);
    nx_user cur, other;
    int rc = nx_catalog_user_by_id(e->cat, u->id, &cur) ? 0 : NX_ENGINE_NOT_FOUND;
    if (rc == 0 && strcmp(cur.username, u->username) != 0 &&
        nx_catalog_user_by_name(e->cat, u->username, &other))
        rc = NX_ENGINE_EXISTS;
    if (rc == 0)
        rc = nx_txn_user(&t, NX_REC_USER_UPDATE, u);
    if (rc == 0)
        rc = log_and_apply(e, &t);
    pthread_mutex_unlock(&e->txn_mu);
    nx_txn_free(&t);
    return rc;
}

/* ===== fsck (I1–I3) ======================================================================= */

typedef struct {
    nx_hmap32 *refs;
    nx_fsck_report *r;
} i2_ctx;

#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
static void fsck_note(nx_fsck_report *r, const char *fmt, ...)
{
    if (r->first_error[0] != '\0')
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->first_error, sizeof r->first_error, fmt, ap);
    va_end(ap);
}

static int i2_check(const nx_index_entry *ent, void *ctx)
{
    i2_ctx *c = ctx;
    c->r->blocks_checked++;
    uint64_t *want = nx_hmap32_get(c->refs, ent->hash);
    uint64_t expect = want ? *want : 0;
    if (want)
        *want |= 1ull << 63; /* mark: seen in the index */
    if ((expect & ~(1ull << 63)) != ent->refcount) {
        char hx[17];
        hex8(ent->hash, hx);
        c->r->i2_errors++;
        fsck_note(c->r, "I2: block %s refcount %" PRIu32 ", recipes reference it %" PRIu64 " times",
                  hx, ent->refcount, expect & ~(1ull << 63));
    }
    return 0;
}

int nx_engine_fsck(nx_engine *e, nx_fsck_report *r)
{
    memset(r, 0, sizeof *r);
    nx_hmap32 refs;
    if (nx_hmap32_init(&refs, 1024) != 0)
        return -1;
    /* Hold txn_mu throughout so nothing commits while we compare. */
    pthread_mutex_lock(&e->txn_mu);
    nx_file *fv = NULL;
    size_t nf = 0;
    int rc = nx_catalog_files(e->cat, NULL, NULL, &fv, &nf);
    /* Files flagged "deleted" (in the trash, undo still possible) still
     * hold their references until FILE_DEL purges them, so all catalog
     * records count (DECISIONS D-017). */
    for (size_t i = 0; i < nf && rc == 0; i++) {
        r->files_checked++;
        nx_chunk_ref *chunks;
        int fr = nx_engine_file_recipe(e, fv[i].id, NULL, &chunks);
        if (fr != 0) {
            if (fr == -1) {
                rc = -1;
                break;
            }
            r->other_errors++;
            fsck_note(r, "file %" PRIu64 ": recipe unreadable", fv[i].id);
            continue;
        }
        for (uint32_t k = 0; k < fv[i].chunk_count; k++) {
            uint64_t *v = nx_hmap32_upsert(&refs, chunks[k].hash, NULL);
            if (!v) {
                rc = -1;
                break;
            }
            (*v)++;
        }
        free(chunks);
        if (rc != 0)
            break;
        /* I1 and I3: read every block (always re-hashed) and the whole file. */
        int rr = read_file_impl(e, fv[i].id, NULL, NULL, true, true);
        r->chunks_checked += fv[i].chunk_count;
        if (rr == NX_ENGINE_CORRUPT) {
            /* Tell I1 (a block) from I3 (the assembled file) apart. */
            bool block_bad = false;
            nx_chunk_ref *c2;
            uint8_t *buf = malloc(NX_CHUNK_MAX_SIZE);
            if (buf && nx_engine_file_recipe(e, fv[i].id, NULL, &c2) == 0) {
                for (uint32_t k = 0; k < fv[i].chunk_count && !block_bad; k++)
                    block_bad = read_block(e, &c2[k], buf, true, true) != 0;
                free(c2);
            }
            free(buf);
            if (block_bad) {
                r->i1_errors++;
                fsck_note(r, "I1: file %" PRIu64 " has a missing or damaged block", fv[i].id);
            } else {
                r->i3_errors++;
                fsck_note(r, "I3: file %" PRIu64 " does not match its SHA-256", fv[i].id);
            }
        } else if (rr != 0) {
            rc = -1;
        }
    }
    free(fv);
    /* I2: every index entry's refcount equals the references counted above,
     * and every referenced block has an entry. */
    if (rc == 0) {
        for (int attempt = 0; attempt < MAX_RETRIES; attempt++) {
            uint64_t blocks = r->blocks_checked, errs = r->i2_errors;
            i2_ctx c = {&refs, r};
            nx_exthash_lock_shared(e->ix);
            int ir = nx_exthash_for_each(e->ix, i2_check, &c);
            nx_exthash_unlock(e->ix);
            if (ir == NX_BP_FULL) {
                r->blocks_checked = blocks;
                r->i2_errors = errs;
                for (size_t i = 0; i < refs.cap; i++)
                    refs.vals[i] &= ~(1ull << 63);
                if (checkpoint_locked(e, e->stable_lsn) != 0) {
                    rc = -1;
                    break;
                }
                continue;
            }
            rc = ir == 0 ? 0 : -1;
            break;
        }
        for (size_t i = 0; rc == 0 && i < refs.cap; i++)
            if (refs.used[i] == 1 && !(refs.vals[i] >> 63)) {
                char hx[17];
                hex8(refs.keys + 32 * i, hx);
                r->i1_errors++;
                fsck_note(r, "I1: block %s is referenced %" PRIu64 " times but not indexed", hx,
                          refs.vals[i]);
            }
    }
    pthread_mutex_unlock(&e->txn_mu);
    nx_hmap32_free(&refs);
    if (rc != 0)
        return -1;
    return (r->i1_errors || r->i2_errors || r->i3_errors || r->other_errors) ? 1 : 0;
}

/* ===== stats ================================================================================ */

void nx_engine_get_stats(nx_engine *e, nx_engine_stats *out)
{
    memset(out, 0, sizeof *out);
    out->files = nx_catalog_file_count(e->cat);
    out->users = nx_catalog_user_count(e->cat);
    out->logical_bytes = atomic_load(&e->logical);
    out->physical_bytes = atomic_load(&e->physical);
    out->unique_blocks = atomic_load(&e->unique_blocks);
    out->writes_skipped_bytes =
        out->logical_bytes > out->physical_bytes ? out->logical_bytes - out->physical_bytes : 0;
    out->network_saved_bytes = atomic_load(&e->saved);
    out->last_lsn = atomic_load(&e->last_lsn);
    out->checkpoint_lsn = atomic_load(&e->ckpt_lsn);
    out->checkpoints = atomic_load(&e->checkpoints);
    out->last_checkpoint_pages = atomic_load(&e->last_ckpt_pages);
    out->last_checkpoint_ms = atomic_load(&e->last_ckpt_ms);
    out->last_checkpoint_at_ms = atomic_load(&e->last_ckpt_at);
    nx_wal_stats ws;
    nx_wal_get_stats(e->wal, &ws);
    out->wal_bytes = ws.bytes;
    out->wal_segments = ws.segments;
    nx_bufpool_get_stats(e->bp, &out->pool);
    nx_exthash_get_stats(e->ix, &out->index);
}

/* ===== storage layout (block map, container stats) =========================================== */

typedef struct {
    nx_block_span *v;
    size_t n, cap;
} span_vec;

static int collect_span(const nx_index_entry *ent, void *ctx)
{
    span_vec *sv = ctx;
    if (sv->n == sv->cap) {
        size_t cap = sv->cap ? sv->cap * 2 : 1024;
        nx_block_span *nv = realloc(sv->v, cap * sizeof *nv);
        if (!nv)
            return -1;
        sv->v = nv;
        sv->cap = cap;
    }
    sv->v[sv->n++] = (nx_block_span){ent->container_id, ent->offset, ent->len, ent->refcount};
    return 0;
}

static int span_cmp(const void *pa, const void *pb)
{
    const nx_block_span *a = pa, *b = pb;
    if (a->container_id != b->container_id)
        return a->container_id < b->container_id ? -1 : 1;
    return a->offset < b->offset ? -1 : a->offset > b->offset;
}

int nx_engine_block_spans(nx_engine *e, nx_block_span **out, size_t *count)
{
    *out = NULL;
    *count = 0;
    for (int attempt = 0; attempt < MAX_RETRIES; attempt++) {
        span_vec sv = {0};
        nx_exthash_lock_shared(e->ix);
        int rc = nx_exthash_for_each(e->ix, collect_span, &sv);
        nx_exthash_unlock(e->ix);
        if (rc == NX_BP_FULL) {
            /* Every frame is dirty (no-steal): checkpoint, then walk again. */
            free(sv.v);
            if (reader_checkpoint(e, false) != 0)
                return -1;
            continue;
        }
        if (rc != 0) {
            free(sv.v);
            return -1;
        }
        if (sv.n > 1)
            qsort(sv.v, sv.n, sizeof *sv.v, span_cmp);
        *out = sv.v;
        *count = sv.n;
        return 0;
    }
    return -1;
}

int nx_engine_containers(nx_engine *e, nx_container_info **out, size_t *count)
{
    *out = NULL;
    *count = 0;
    uint32_t *ids;
    size_t n;
    if (nx_containers_list(e->ct, &ids, &n) != 0)
        return -1;
    nx_container_info *v = n ? malloc(n * sizeof *v) : NULL;
    if (n && !v) {
        free(ids);
        return -1;
    }
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        uint64_t size;
        /* A container dropped by compaction between list and stat is skipped. */
        if (nx_containers_file_size(e->ct, ids[i], &size) != 0)
            continue;
        v[k++] = (nx_container_info){ids[i], size};
    }
    free(ids);
    *out = v;
    *count = k;
    return 0;
}
