/*
 * bufpool.c — see bufpool.h.
 *
 * Locks: `mu` protects the page table, pin counts, dirty flags, and the
 * clock hand. It is a leaf below the frame latches (SPEC §9 #5): a thread
 * may hold a frame latch and then take `mu` (pin another page, mark
 * dirty), never the other way round. On a miss the page is read from disk
 * while holding `mu` ("never held across I/O except the page read itself").
 */
#include "engine/bufpool.h"

#include "util/bytes.h"
#include "util/crc32.h"
#include "util/log.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct nx_frame {
    uint8_t *data;
    uint32_t file_id, page_no;
    bool valid, dirty, ref;
    int pins;
    int32_t hnext; /* page-table chain */
    pthread_rwlock_t latch;
};

struct nx_bufpool {
    pthread_mutex_t mu;
    nx_frame *frames;
    uint8_t *memory;
    size_t nframes;
    int32_t *heads;
    size_t nheads; /* power of two */
    size_t hand;
    size_t dirty;
    plat_file *files[NX_BP_MAX_FILES];
    uint64_t hits, misses, evictions;
};

/* ---- page header helpers ------------------------------------------------- */

uint64_t nx_page_lsn(const uint8_t *page) { return nx_get_u64(page); }
void nx_page_set_lsn(uint8_t *page, uint64_t lsn) { nx_put_u64(page, lsn); }

static uint32_t page_crc(const uint8_t *page)
{
    uint32_t c = nx_crc32(0, page, NX_PAGE_CRC_OFF);
    return nx_crc32(c, page + NX_PAGE_CRC_OFF + 4, NX_PAGE_SIZE - NX_PAGE_CRC_OFF - 4);
}

void nx_page_seal_crc(uint8_t *page) { nx_put_u32(page + NX_PAGE_CRC_OFF, page_crc(page)); }

bool nx_page_crc_ok(const uint8_t *page)
{
    return nx_get_u32(page + NX_PAGE_CRC_OFF) == page_crc(page);
}

static bool all_zero(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (p[i])
            return false;
    return true;
}

/* ---- page table --------------------------------------------------------------- */

static size_t slot_of(const nx_bufpool *bp, uint32_t file_id, uint32_t page_no)
{
    uint64_t k = ((uint64_t)file_id << 32) | page_no;
    k *= 0x9E3779B97F4A7C15ull; /* Fibonacci hashing */
    return (size_t)(k >> 32) & (bp->nheads - 1);
}

static int32_t table_find(const nx_bufpool *bp, uint32_t file_id, uint32_t page_no)
{
    for (int32_t i = bp->heads[slot_of(bp, file_id, page_no)]; i >= 0; i = bp->frames[i].hnext)
        if (bp->frames[i].file_id == file_id && bp->frames[i].page_no == page_no)
            return i;
    return -1;
}

static void table_insert(nx_bufpool *bp, int32_t idx)
{
    size_t s = slot_of(bp, bp->frames[idx].file_id, bp->frames[idx].page_no);
    bp->frames[idx].hnext = bp->heads[s];
    bp->heads[s] = idx;
}

static void table_remove(nx_bufpool *bp, int32_t idx)
{
    size_t s = slot_of(bp, bp->frames[idx].file_id, bp->frames[idx].page_no);
    int32_t *link = &bp->heads[s];
    while (*link >= 0) {
        if (*link == idx) {
            *link = bp->frames[idx].hnext;
            return;
        }
        link = &bp->frames[*link].hnext;
    }
}

/* ---- lifecycle ------------------------------------------------------------------ */

int nx_bufpool_create(size_t frames, nx_bufpool **out)
{
    if (frames < 8 || frames > (1u << 24))
        return -1;
    nx_bufpool *bp = calloc(1, sizeof *bp);
    if (!bp)
        return -1;
    bp->nframes = frames;
    bp->nheads = 16;
    while (bp->nheads < frames * 2)
        bp->nheads <<= 1;
    bp->frames = calloc(frames, sizeof *bp->frames);
    bp->memory = malloc(frames * NX_PAGE_SIZE);
    bp->heads = malloc(bp->nheads * sizeof *bp->heads);
    if (!bp->frames || !bp->memory || !bp->heads) {
        free(bp->frames);
        free(bp->memory);
        free(bp->heads);
        free(bp);
        return -1;
    }
    for (size_t i = 0; i < bp->nheads; i++)
        bp->heads[i] = -1;
    for (size_t i = 0; i < frames; i++) {
        bp->frames[i].data = bp->memory + i * NX_PAGE_SIZE;
        bp->frames[i].hnext = -1;
        pthread_rwlock_init(&bp->frames[i].latch, NULL);
    }
    pthread_mutex_init(&bp->mu, NULL);
    *out = bp;
    return 0;
}

void nx_bufpool_destroy(nx_bufpool *bp)
{
    if (!bp)
        return;
    for (size_t i = 0; i < bp->nframes; i++)
        pthread_rwlock_destroy(&bp->frames[i].latch);
    pthread_mutex_destroy(&bp->mu);
    free(bp->frames);
    free(bp->memory);
    free(bp->heads);
    free(bp);
}

int nx_bufpool_attach(nx_bufpool *bp, uint32_t file_id, plat_file *f)
{
    if (file_id >= NX_BP_MAX_FILES)
        return -1;
    pthread_mutex_lock(&bp->mu);
    bp->files[file_id] = f;
    pthread_mutex_unlock(&bp->mu);
    return 0;
}

/* ---- pin / unpin -------------------------------------------------------------------- */

/* Clock: two full sweeps are enough to clear every reference bit once. */
static int32_t find_victim(nx_bufpool *bp)
{
    for (size_t step = 0; step < 2 * bp->nframes; step++) {
        size_t i = bp->hand;
        bp->hand = (bp->hand + 1) % bp->nframes;
        nx_frame *f = &bp->frames[i];
        if (!f->valid)
            return (int32_t)i;
        if (f->pins > 0 || f->dirty) /* no-steal: dirty frames stay */
            continue;
        if (f->ref) {
            f->ref = false; /* second chance */
            continue;
        }
        return (int32_t)i;
    }
    return -1;
}

int nx_bufpool_pin(nx_bufpool *bp, uint32_t file_id, uint32_t page_no, int mode, nx_frame **out)
{
    if (file_id >= NX_BP_MAX_FILES)
        return -1;
    pthread_mutex_lock(&bp->mu);
    int32_t idx = table_find(bp, file_id, page_no);
    if (idx >= 0) {
        nx_frame *f = &bp->frames[idx];
        f->pins++;
        f->ref = true;
        bp->hits++;
        if (mode == NX_PIN_NEW)
            memset(f->data, 0, NX_PAGE_SIZE); /* a re-allocated page id starts empty */
        pthread_mutex_unlock(&bp->mu);
        *out = f;
        return 0;
    }
    plat_file *file = bp->files[file_id];
    if (!file) {
        pthread_mutex_unlock(&bp->mu);
        return -1;
    }
    int32_t v = find_victim(bp);
    if (v < 0) {
        pthread_mutex_unlock(&bp->mu);
        return NX_BP_FULL;
    }
    nx_frame *f = &bp->frames[v];
    if (f->valid) {
        table_remove(bp, v);
        bp->evictions++;
    }
    f->valid = false;
    if (mode == NX_PIN_NEW) {
        memset(f->data, 0, NX_PAGE_SIZE);
    } else {
        bp->misses++;
        int64_t got = plat_pread(file, f->data, NX_PAGE_SIZE, (uint64_t)page_no * NX_PAGE_SIZE);
        if (got < 0) {
            pthread_mutex_unlock(&bp->mu);
            LOG_ERROR("bufpool", "read failed file=%" PRIu32 " page=%" PRIu32 " err=\"%s\"",
                      file_id, page_no, plat_last_error());
            return -1;
        }
        if (got < (int64_t)NX_PAGE_SIZE)
            memset(f->data + got, 0, NX_PAGE_SIZE - (size_t)got); /* beyond EOF: fresh page */
        if (!all_zero(f->data, NX_PAGE_SIZE) && !nx_page_crc_ok(f->data)) {
            pthread_mutex_unlock(&bp->mu);
            LOG_ERROR("bufpool", "page checksum mismatch file=%" PRIu32 " page=%" PRIu32, file_id,
                      page_no);
            return NX_BP_CORRUPT;
        }
    }
    f->file_id = file_id;
    f->page_no = page_no;
    f->valid = true;
    f->dirty = false;
    f->ref = true;
    f->pins = 1;
    table_insert(bp, v);
    pthread_mutex_unlock(&bp->mu);
    *out = f;
    return 0;
}

void nx_bufpool_unpin(nx_bufpool *bp, nx_frame *fr)
{
    pthread_mutex_lock(&bp->mu);
    if (fr->pins > 0)
        fr->pins--;
    pthread_mutex_unlock(&bp->mu);
}

uint8_t *nx_frame_data(nx_frame *fr) { return fr->data; }
uint32_t nx_frame_page_no(const nx_frame *fr) { return fr->page_no; }
void nx_frame_rdlock(nx_frame *fr) { pthread_rwlock_rdlock(&fr->latch); }
void nx_frame_wrlock(nx_frame *fr) { pthread_rwlock_wrlock(&fr->latch); }
void nx_frame_unlock(nx_frame *fr) { pthread_rwlock_unlock(&fr->latch); }

void nx_bufpool_mark_dirty(nx_bufpool *bp, nx_frame *fr, uint64_t lsn)
{
    if (lsn > nx_page_lsn(fr->data))
        nx_page_set_lsn(fr->data, lsn);
    pthread_mutex_lock(&bp->mu);
    if (!fr->dirty) {
        fr->dirty = true;
        bp->dirty++;
    }
    pthread_mutex_unlock(&bp->mu);
}

/* ---- checkpoint support ---------------------------------------------------------------- */

int nx_bufpool_for_each_dirty(nx_bufpool *bp, nx_dirty_cb cb, void *ctx)
{
    /* Snapshot the dirty set; dirty frames cannot be evicted, so their
     * identity stays stable while we copy them. */
    pthread_mutex_lock(&bp->mu);
    size_t n = 0;
    int32_t *list = malloc((bp->dirty ? bp->dirty : 1) * sizeof *list);
    if (!list) {
        pthread_mutex_unlock(&bp->mu);
        return -1;
    }
    for (size_t i = 0; i < bp->nframes && n < bp->dirty; i++)
        if (bp->frames[i].valid && bp->frames[i].dirty)
            list[n++] = (int32_t)i;
    pthread_mutex_unlock(&bp->mu);

    uint8_t *copy = malloc(NX_PAGE_SIZE);
    if (!copy) {
        free(list);
        return -1;
    }
    int rc = 0;
    for (size_t k = 0; k < n && rc == 0; k++) {
        nx_frame *f = &bp->frames[list[k]];
        nx_frame_rdlock(f);
        memcpy(copy, f->data, NX_PAGE_SIZE);
        uint32_t file_id = f->file_id, page_no = f->page_no;
        nx_frame_unlock(f);
        nx_page_seal_crc(copy);
        rc = cb(file_id, page_no, copy, ctx);
    }
    free(copy);
    free(list);
    return rc;
}

int nx_bufpool_write_page(nx_bufpool *bp, uint32_t file_id, uint32_t page_no, const uint8_t *page)
{
    if (file_id >= NX_BP_MAX_FILES || !bp->files[file_id])
        return -1;
    if (plat_pwrite_all(bp->files[file_id], page, NX_PAGE_SIZE, (uint64_t)page_no * NX_PAGE_SIZE) !=
        0) {
        LOG_ERROR("bufpool", "write failed file=%" PRIu32 " page=%" PRIu32 " err=\"%s\"", file_id,
                  page_no, plat_last_error());
        return -1;
    }
    return 0;
}

int nx_bufpool_sync_files(nx_bufpool *bp)
{
    for (uint32_t i = 0; i < NX_BP_MAX_FILES; i++) {
        if (bp->files[i] && plat_fsync_file(bp->files[i]) != 0) {
            LOG_ERROR("bufpool", "fsync failed file=%" PRIu32 " err=\"%s\"", i, plat_last_error());
            return -1;
        }
    }
    return 0;
}

void nx_bufpool_mark_all_clean(nx_bufpool *bp)
{
    pthread_mutex_lock(&bp->mu);
    for (size_t i = 0; i < bp->nframes; i++)
        bp->frames[i].dirty = false;
    bp->dirty = 0;
    pthread_mutex_unlock(&bp->mu);
}

static int write_cb(uint32_t file_id, uint32_t page_no, const uint8_t *page, void *ctx)
{
    return nx_bufpool_write_page(ctx, file_id, page_no, page);
}

int nx_bufpool_flush_simple(nx_bufpool *bp)
{
    if (nx_bufpool_for_each_dirty(bp, write_cb, bp) != 0 || nx_bufpool_sync_files(bp) != 0)
        return -1;
    nx_bufpool_mark_all_clean(bp);
    return 0;
}

void nx_bufpool_drop_file(nx_bufpool *bp, uint32_t file_id)
{
    pthread_mutex_lock(&bp->mu);
    for (size_t i = 0; i < bp->nframes; i++) {
        nx_frame *f = &bp->frames[i];
        if (f->valid && f->file_id == file_id && f->pins == 0) {
            table_remove(bp, (int32_t)i);
            if (f->dirty)
                bp->dirty--;
            f->valid = false;
            f->dirty = false;
        }
    }
    pthread_mutex_unlock(&bp->mu);
}

void nx_bufpool_get_stats(nx_bufpool *bp, nx_bufpool_stats *out)
{
    pthread_mutex_lock(&bp->mu);
    out->hits = bp->hits;
    out->misses = bp->misses;
    out->evictions = bp->evictions;
    out->frames = bp->nframes;
    out->dirty = bp->dirty;
    size_t pinned = 0;
    for (size_t i = 0; i < bp->nframes; i++)
        pinned += bp->frames[i].pins > 0;
    out->pinned = pinned;
    pthread_mutex_unlock(&bp->mu);
}

size_t nx_bufpool_dirty_count(nx_bufpool *bp)
{
    pthread_mutex_lock(&bp->mu);
    size_t d = bp->dirty;
    pthread_mutex_unlock(&bp->mu);
    return d;
}

size_t nx_bufpool_frame_count(nx_bufpool *bp) { return bp->nframes; }
