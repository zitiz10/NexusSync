/*
 * bufpool.h — the buffer pool: a fixed set of 4096-byte frames caching
 * pages of the index and catalog files (SPEC §7.3).
 *
 * - Clock replacement: a "hand" sweeps the frames; a frame used since the
 *   last sweep gets a second chance (its reference bit is cleared).
 * - Pin counts: a pinned frame is in use and is never evicted.
 * - No-steal: a dirty frame is never written out except by a checkpoint,
 *   so the files on disk always hold the state of the last checkpoint.
 *   If every frame is pinned or dirty, pin() returns NX_BP_FULL and the
 *   caller runs a checkpoint and retries (the pool never blocks).
 * - Per-frame latch (rwlock): protects the page bytes while reading or
 *   changing them. Separate from the pin count.
 *
 * Every page shares a common header so the pool can checksum it:
 *   [0..8) u64 page_lsn   [8..12) owner-specific   [12..16) u32 crc32
 * The CRC covers bytes [0..12) and [16..4096) and is set when the page is
 * written out; it is checked on every read from disk.
 */
#ifndef NX_BUFPOOL_H
#define NX_BUFPOOL_H

#include "platform/platform.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NX_PAGE_SIZE 4096u
#define NX_PAGE_CRC_OFF 12u
#define NX_BP_MAX_FILES 8u

#define NX_BP_FULL (-2)    /* no clean, unpinned frame: checkpoint, then retry */
#define NX_BP_CORRUPT (-3) /* page on disk failed its CRC */

enum { NX_PIN_READ = 0, NX_PIN_NEW = 1 /* fresh zeroed page, do not read */ };

typedef struct nx_bufpool nx_bufpool;
typedef struct nx_frame nx_frame;

int nx_bufpool_create(size_t frames, nx_bufpool **out);
void nx_bufpool_destroy(nx_bufpool *bp);
/* Registers a page file under a small id (index buckets, users, files...).
 * The pool does not own the handle. */
int nx_bufpool_attach(nx_bufpool *bp, uint32_t file_id, plat_file *f);

int nx_bufpool_pin(nx_bufpool *bp, uint32_t file_id, uint32_t page_no, int mode, nx_frame **out);
void nx_bufpool_unpin(nx_bufpool *bp, nx_frame *fr);

uint8_t *nx_frame_data(nx_frame *fr);
uint32_t nx_frame_page_no(const nx_frame *fr);
void nx_frame_rdlock(nx_frame *fr);
void nx_frame_wrlock(nx_frame *fr);
void nx_frame_unlock(nx_frame *fr);

/* Caller holds the frame's write latch. Records lsn as the page_lsn. */
void nx_bufpool_mark_dirty(nx_bufpool *bp, nx_frame *fr, uint64_t lsn);

uint64_t nx_page_lsn(const uint8_t *page);
void nx_page_set_lsn(uint8_t *page, uint64_t lsn);
void nx_page_seal_crc(uint8_t *page);
bool nx_page_crc_ok(const uint8_t *page);

/* ---- checkpoint support (SPEC §8). Callers exclude writers. ----------- */

/* Calls cb with a CRC-sealed copy of every dirty page. */
typedef int (*nx_dirty_cb)(uint32_t file_id, uint32_t page_no, const uint8_t *page, void *ctx);
int nx_bufpool_for_each_dirty(nx_bufpool *bp, nx_dirty_cb cb, void *ctx);
/* Writes one (already sealed) page image to its file at page_no * 4096. */
int nx_bufpool_write_page(nx_bufpool *bp, uint32_t file_id, uint32_t page_no, const uint8_t *page);
/* fsyncs every attached file. */
int nx_bufpool_sync_files(nx_bufpool *bp);
void nx_bufpool_mark_all_clean(nx_bufpool *bp);
/* Write every dirty page in place, fsync, mark clean. No doublewrite:
 * only for tests and tools; the engine checkpoint uses the doublewrite. */
int nx_bufpool_flush_simple(nx_bufpool *bp);
/* Forgets every cached page of a file (after it was rewritten on disk). */
void nx_bufpool_drop_file(nx_bufpool *bp, uint32_t file_id);

typedef struct {
    uint64_t hits, misses, evictions;
    size_t frames, dirty, pinned;
} nx_bufpool_stats;

void nx_bufpool_get_stats(nx_bufpool *bp, nx_bufpool_stats *out);
size_t nx_bufpool_dirty_count(nx_bufpool *bp);
size_t nx_bufpool_frame_count(nx_bufpool *bp);

#endif
