/* exthash.c — extendible hashing block index. See exthash.h. */
#include "engine/exthash.h"

#include "util/bytes.h"
#include "util/crc32.h"
#include "util/log.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define DIR_VERSION 1u

struct nx_exthash {
    nx_bufpool *bp;
    uint32_t file_id;
    pthread_rwlock_t dir_lock; /* SPEC §9 #3 */
    uint32_t gd;               /* global depth */
    uint32_t *dir;             /* 2^gd page ids */
    uint32_t page_count;
    bool dir_dirty;
    atomic_uint_fast64_t entries;
};

/* ---- bucket page accessors ------------------------------------------------- */

static uint16_t b_depth(const uint8_t *p) { return nx_get_u16(p + 8); }
static uint16_t b_count(const uint8_t *p) { return nx_get_u16(p + 10); }
static void b_set_depth(uint8_t *p, uint16_t d) { nx_put_u16(p + 8, d); }
static void b_set_count(uint8_t *p, uint16_t n) { nx_put_u16(p + 10, n); }
static uint8_t *b_entry(uint8_t *p, unsigned i) { return p + NX_BUCKET_HEADER + i * NX_INDEX_ENTRY_SIZE; }

static void entry_read(const uint8_t *e, nx_index_entry *out)
{
    memcpy(out->hash, e, 32);
    out->container_id = nx_get_u32(e + 32);
    out->offset = nx_get_u32(e + 36);
    out->len = nx_get_u32(e + 40);
    out->refcount = nx_get_u32(e + 44);
    out->created_at = nx_get_u32(e + 48);
}

static void entry_write(uint8_t *e, const nx_index_entry *in)
{
    memcpy(e, in->hash, 32);
    nx_put_u32(e + 32, in->container_id);
    nx_put_u32(e + 36, in->offset);
    nx_put_u32(e + 40, in->len);
    nx_put_u32(e + 44, in->refcount);
    nx_put_u32(e + 48, in->created_at);
}

static int find_in_page(uint8_t *p, const uint8_t hash[32])
{
    unsigned n = b_count(p);
    for (unsigned i = 0; i < n; i++)
        if (memcmp(b_entry(p, i), hash, 32) == 0)
            return (int)i;
    return -1;
}

static uint32_t top32(const uint8_t hash[32])
{
    return ((uint32_t)hash[0] << 24) | ((uint32_t)hash[1] << 16) | ((uint32_t)hash[2] << 8) |
           (uint32_t)hash[3];
}

uint32_t nx_exthash_slot(const uint8_t hash[32], uint32_t depth)
{
    return depth == 0 ? 0 : top32(hash) >> (32u - depth);
}

/* ---- open / close ------------------------------------------------------------------ */

static int pin(nx_exthash *ex, uint32_t page, int mode, nx_frame **fr)
{
    return nx_bufpool_pin(ex->bp, ex->file_id, page, mode, fr);
}

static int load_dir(nx_exthash *ex, const uint8_t *img, size_t len)
{
    if (len < 16 || memcmp(img, "NXDR", 4) != 0 || nx_get_u16(img + 4) != DIR_VERSION)
        return -1;
    uint32_t gd = nx_get_u32(img + 8);
    if (gd > NX_EXT_MAX_DEPTH)
        return -1;
    size_t n = (size_t)1 << gd, body = 12 + 4 * n;
    if (len < body + 4 || nx_crc32(0, img, body) != nx_get_u32(img + body))
        return -1;
    ex->dir = malloc(n * sizeof *ex->dir);
    if (!ex->dir)
        return -1;
    uint32_t max = 0;
    for (size_t i = 0; i < n; i++) {
        ex->dir[i] = nx_get_u32(img + 12 + 4 * i);
        if (ex->dir[i] > max)
            max = ex->dir[i];
    }
    if (max >= n) { /* there are never more pages than directory slots */
        free(ex->dir);
        ex->dir = NULL;
        return -1;
    }
    ex->gd = gd;
    ex->page_count = max + 1;
    return 0;
}

int nx_exthash_open(nx_bufpool *bp, uint32_t file_id, const uint8_t *dir_image, size_t dir_len,
                    nx_exthash **out)
{
    nx_exthash *ex = calloc(1, sizeof *ex);
    if (!ex)
        return -1;
    ex->bp = bp;
    ex->file_id = file_id;
    pthread_rwlock_init(&ex->dir_lock, NULL);
    atomic_init(&ex->entries, 0);

    if (dir_image && dir_len > 0) {
        if (load_dir(ex, dir_image, dir_len) != 0) {
            LOG_ERROR("index", "dir.bin is invalid");
            nx_exthash_close(ex);
            return -1;
        }
        /* Count entries and verify every bucket page's checksum once. */
        for (uint32_t p = 0; p < ex->page_count; p++) {
            nx_frame *fr;
            int rc = pin(ex, p, NX_PIN_READ, &fr);
            if (rc != 0) {
                LOG_ERROR("index", "cannot read bucket page=%" PRIu32 " rc=%d", p, rc);
                nx_exthash_close(ex);
                return -1;
            }
            atomic_fetch_add(&ex->entries, b_count(nx_frame_data(fr)));
            nx_bufpool_unpin(bp, fr);
        }
    } else {
        /* New index: global depth 4, every slot points at one empty page
         * with local depth 0. The first splits need no directory doubling. */
        size_t n = (size_t)1 << NX_EXT_INITIAL_DEPTH;
        ex->dir = calloc(n, sizeof *ex->dir);
        if (!ex->dir) {
            nx_exthash_close(ex);
            return -1;
        }
        ex->gd = NX_EXT_INITIAL_DEPTH;
        ex->page_count = 1;
        nx_frame *fr;
        if (pin(ex, 0, NX_PIN_NEW, &fr) != 0) {
            nx_exthash_close(ex);
            return -1;
        }
        nx_frame_wrlock(fr);
        b_set_depth(nx_frame_data(fr), 0);
        b_set_count(nx_frame_data(fr), 0);
        nx_bufpool_mark_dirty(bp, fr, 0);
        nx_frame_unlock(fr);
        nx_bufpool_unpin(bp, fr);
        ex->dir_dirty = true;
    }
    *out = ex;
    return 0;
}

void nx_exthash_close(nx_exthash *ex)
{
    if (!ex)
        return;
    pthread_rwlock_destroy(&ex->dir_lock);
    free(ex->dir);
    free(ex);
}

void nx_exthash_lock_shared(nx_exthash *ex) { pthread_rwlock_rdlock(&ex->dir_lock); }
void nx_exthash_lock_exclusive(nx_exthash *ex) { pthread_rwlock_wrlock(&ex->dir_lock); }
void nx_exthash_unlock(nx_exthash *ex) { pthread_rwlock_unlock(&ex->dir_lock); }

/* ---- operations ------------------------------------------------------------------------ */

static uint32_t page_for(const nx_exthash *ex, const uint8_t hash[32])
{
    return ex->dir[nx_exthash_slot(hash, ex->gd)];
}

int nx_exthash_lookup(nx_exthash *ex, const uint8_t hash[32], nx_index_entry *out)
{
    nx_frame *fr;
    int rc = pin(ex, page_for(ex, hash), NX_PIN_READ, &fr);
    if (rc != 0)
        return rc;
    nx_frame_rdlock(fr);
    uint8_t *p = nx_frame_data(fr);
    int i = find_in_page(p, hash);
    if (i >= 0 && out)
        entry_read(b_entry(p, (unsigned)i), out);
    nx_frame_unlock(fr);
    nx_bufpool_unpin(ex->bp, fr);
    return i >= 0 ? 0 : NX_EXT_NOT_FOUND;
}

/* Pins and write-latches the bucket for hash; applies the redo check. */
static int open_for_write(nx_exthash *ex, const uint8_t hash[32], uint64_t lsn, bool redo,
                          nx_frame **fr)
{
    int rc = pin(ex, page_for(ex, hash), NX_PIN_READ, fr);
    if (rc != 0)
        return rc;
    nx_frame_wrlock(*fr);
    if (redo && nx_page_lsn(nx_frame_data(*fr)) >= lsn) {
        nx_frame_unlock(*fr);
        nx_bufpool_unpin(ex->bp, *fr);
        return NX_EXT_SKIPPED;
    }
    return 0;
}

static void close_write(nx_exthash *ex, nx_frame *fr, bool changed, uint64_t lsn)
{
    if (changed)
        nx_bufpool_mark_dirty(ex->bp, fr, lsn);
    nx_frame_unlock(fr);
    nx_bufpool_unpin(ex->bp, fr);
}

int nx_exthash_insert(nx_exthash *ex, const nx_index_entry *e, uint64_t lsn, bool redo)
{
    nx_frame *fr;
    int rc = open_for_write(ex, e->hash, lsn, redo, &fr);
    if (rc != 0)
        return rc;
    uint8_t *p = nx_frame_data(fr);
    if (find_in_page(p, e->hash) >= 0) {
        close_write(ex, fr, false, lsn);
        return NX_EXT_EXISTS;
    }
    unsigned n = b_count(p);
    if (n >= NX_BUCKET_CAPACITY) {
        close_write(ex, fr, false, lsn);
        return NX_EXT_NEED_SPLIT;
    }
    entry_write(b_entry(p, n), e);
    b_set_count(p, (uint16_t)(n + 1));
    close_write(ex, fr, true, lsn);
    atomic_fetch_add(&ex->entries, 1);
    return 0;
}

int nx_exthash_add_ref(nx_exthash *ex, const uint8_t hash[32], int64_t delta, uint64_t lsn,
                       bool redo, uint32_t *new_ref)
{
    nx_frame *fr;
    int rc = open_for_write(ex, hash, lsn, redo, &fr);
    if (rc != 0)
        return rc;
    uint8_t *p = nx_frame_data(fr);
    int i = find_in_page(p, hash);
    if (i < 0) {
        close_write(ex, fr, false, lsn);
        return NX_EXT_NOT_FOUND;
    }
    uint8_t *e = b_entry(p, (unsigned)i);
    int64_t v = (int64_t)nx_get_u32(e + 44) + delta;
    if (v < 0 || v > (int64_t)UINT32_MAX) {
        close_write(ex, fr, false, lsn);
        return NX_EXT_UNDERFLOW;
    }
    nx_put_u32(e + 44, (uint32_t)v);
    if (new_ref)
        *new_ref = (uint32_t)v;
    close_write(ex, fr, true, lsn);
    return 0;
}

int nx_exthash_set_location(nx_exthash *ex, const uint8_t hash[32], uint32_t container_id,
                            uint32_t offset, uint64_t lsn, bool redo)
{
    nx_frame *fr;
    int rc = open_for_write(ex, hash, lsn, redo, &fr);
    if (rc != 0)
        return rc;
    uint8_t *p = nx_frame_data(fr);
    int i = find_in_page(p, hash);
    if (i < 0) {
        close_write(ex, fr, false, lsn);
        return NX_EXT_NOT_FOUND;
    }
    uint8_t *e = b_entry(p, (unsigned)i);
    nx_put_u32(e + 32, container_id);
    nx_put_u32(e + 36, offset);
    close_write(ex, fr, true, lsn);
    return 0;
}

int nx_exthash_remove(nx_exthash *ex, const uint8_t hash[32], uint64_t lsn, bool redo)
{
    nx_frame *fr;
    int rc = open_for_write(ex, hash, lsn, redo, &fr);
    if (rc != 0)
        return rc;
    uint8_t *p = nx_frame_data(fr);
    int i = find_in_page(p, hash);
    if (i < 0) {
        close_write(ex, fr, false, lsn);
        return NX_EXT_NOT_FOUND;
    }
    unsigned n = b_count(p);
    /* Keep entries packed: move the last one into the hole. */
    if ((unsigned)i != n - 1)
        memcpy(b_entry(p, (unsigned)i), b_entry(p, n - 1), NX_INDEX_ENTRY_SIZE);
    memset(b_entry(p, n - 1), 0, NX_INDEX_ENTRY_SIZE);
    b_set_count(p, (uint16_t)(n - 1));
    close_write(ex, fr, true, lsn);
    atomic_fetch_sub(&ex->entries, 1);
    return 0;
}

static int double_directory(nx_exthash *ex)
{
    if (ex->gd >= NX_EXT_MAX_DEPTH) {
        LOG_ERROR("index", "directory at maximum depth=%" PRIu32, ex->gd);
        return -1;
    }
    size_t n = (size_t)1 << ex->gd;
    uint32_t *nd = malloc(2 * n * sizeof *nd);
    if (!nd)
        return -1;
    /* Slot i at depth d+1 has the same top d bits as slot i>>1 at depth d. */
    for (size_t i = 0; i < 2 * n; i++)
        nd[i] = ex->dir[i >> 1];
    free(ex->dir);
    ex->dir = nd;
    ex->gd++;
    ex->dir_dirty = true;
    LOG_DEBUG("index", "directory doubled global_depth=%" PRIu32, ex->gd);
    return 0;
}

int nx_exthash_split(nx_exthash *ex, const uint8_t hash[32])
{
    /* A split is structural: it moves entries but contains no logged change.
     * Both pages therefore keep the old page's LSN. Stamping them with the
     * LSN of the record being applied would make redo skip that very record
     * when it is retried after the split (the insert would be lost). */
    uint32_t old_id = page_for(ex, hash);
    nx_frame *old_fr, *new_fr;
    int rc = pin(ex, old_id, NX_PIN_READ, &old_fr);
    if (rc != 0)
        return rc;
    nx_frame_wrlock(old_fr);
    uint8_t *op = nx_frame_data(old_fr);
    if (b_count(op) < NX_BUCKET_CAPACITY) { /* already split by someone else */
        close_write(ex, old_fr, false, 0);
        return 0;
    }
    uint32_t ld = b_depth(op);
    if (ld == ex->gd && double_directory(ex) != 0) {
        close_write(ex, old_fr, false, 0);
        return -1;
    }
    uint32_t new_id = ex->page_count;
    rc = pin(ex, new_id, NX_PIN_NEW, &new_fr);
    if (rc != 0) {
        close_write(ex, old_fr, false, 0);
        return rc;
    }
    nx_frame_wrlock(new_fr);
    uint8_t *np = nx_frame_data(new_fr);
    ex->page_count++;

    /* Entries whose bit number `ld` (counting from the top) is 1 move. */
    unsigned n = b_count(op), keep = 0, moved = 0;
    for (unsigned i = 0; i < n; i++) {
        uint8_t *e = b_entry(op, i);
        if ((top32(e) >> (31u - ld)) & 1u)
            memcpy(b_entry(np, moved++), e, NX_INDEX_ENTRY_SIZE);
        else
            memmove(b_entry(op, keep++), e, NX_INDEX_ENTRY_SIZE);
    }
    memset(b_entry(op, keep), 0, (n - keep) * NX_INDEX_ENTRY_SIZE);
    b_set_count(op, (uint16_t)keep);
    b_set_count(np, (uint16_t)moved);
    b_set_depth(op, (uint16_t)(ld + 1));
    b_set_depth(np, (uint16_t)(ld + 1));
    /* The new page holds state copied from the old one, so it carries the
     * old page's LSN as well (redo reasons per page). */
    nx_page_set_lsn(np, nx_page_lsn(op));

    /* Repoint the slots of the old page whose bit `ld` is 1. */
    size_t slots = (size_t)1 << ex->gd;
    for (size_t i = 0; i < slots; i++)
        if (ex->dir[i] == old_id && ((i >> (ex->gd - 1u - ld)) & 1u))
            ex->dir[i] = new_id;
    ex->dir_dirty = true;

    uint64_t keep_lsn = nx_page_lsn(op);
    close_write(ex, new_fr, true, keep_lsn);
    close_write(ex, old_fr, true, keep_lsn);
    return 0;
}

int nx_exthash_for_each(nx_exthash *ex, nx_index_cb cb, void *ctx)
{
    nx_index_entry *batch = malloc(NX_BUCKET_CAPACITY * sizeof *batch);
    if (!batch)
        return -1;
    int rc = 0;
    for (uint32_t pg = 0; pg < ex->page_count && rc == 0; pg++) {
        nx_frame *fr;
        rc = pin(ex, pg, NX_PIN_READ, &fr);
        if (rc != 0)
            break;
        nx_frame_rdlock(fr);
        uint8_t *p = nx_frame_data(fr);
        unsigned n = b_count(p);
        for (unsigned i = 0; i < n; i++)
            entry_read(b_entry(p, i), &batch[i]);
        nx_frame_unlock(fr);
        nx_bufpool_unpin(ex->bp, fr);
        /* Callbacks run without a latch held, so they may use the pool. */
        for (unsigned i = 0; i < n && rc == 0; i++)
            rc = cb(&batch[i], ctx);
    }
    free(batch);
    return rc;
}

int nx_exthash_dir_image(nx_exthash *ex, uint8_t **buf, size_t *len)
{
    size_t n = (size_t)1 << ex->gd, body = 12 + 4 * n;
    size_t total = (body + 4 + NX_PAGE_SIZE - 1) / NX_PAGE_SIZE * NX_PAGE_SIZE;
    uint8_t *img = calloc(1, total);
    if (!img)
        return -1;
    memcpy(img, "NXDR", 4);
    nx_put_u16(img + 4, DIR_VERSION);
    nx_put_u32(img + 8, ex->gd);
    for (size_t i = 0; i < n; i++)
        nx_put_u32(img + 12 + 4 * i, ex->dir[i]);
    nx_put_u32(img + body, nx_crc32(0, img, body));
    *buf = img;
    *len = total;
    return 0;
}

bool nx_exthash_dir_dirty(nx_exthash *ex, bool clear)
{
    bool d = ex->dir_dirty;
    if (clear)
        ex->dir_dirty = false;
    return d;
}

/* gd and page_count change during a split (exclusive lock), and stats are
 * read from other threads (the server's background thread), so read them
 * under the shared lock. Callers must not already hold the lock. */
void nx_exthash_get_stats(nx_exthash *ex, nx_exthash_stats *out)
{
    pthread_rwlock_rdlock(&ex->dir_lock);
    out->entries = atomic_load(&ex->entries);
    out->global_depth = ex->gd;
    out->buckets = ex->page_count;
    pthread_rwlock_unlock(&ex->dir_lock);
    out->avg_fill_percent =
        out->buckets ? 100.0 * (double)out->entries / ((double)out->buckets * NX_BUCKET_CAPACITY)
                     : 0.0;
}

uint32_t nx_exthash_bucket_count(nx_exthash *ex)
{
    pthread_rwlock_rdlock(&ex->dir_lock);
    uint32_t n = ex->page_count;
    pthread_rwlock_unlock(&ex->dir_lock);
    return n;
}
