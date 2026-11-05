/*
 * exthash.h — the block index: extendible hashing over bucket pages
 * (SPEC §5, §7.2). Maps a block's SHA-256 to where its bytes live and how
 * many file references it has.
 *
 * Directory: 2^global_depth page ids, indexed by the top global_depth bits
 * of the hash. Several slots may share one bucket page; a page's
 * local_depth says how many top bits its entries have in common.
 *
 * Insert into a full bucket: if local_depth < global_depth, split the page
 * into two by the next bit and repoint half of its directory slots;
 * otherwise double the directory first. Buckets never merge.
 *
 * Lookup cost: one directory read (in memory) + one bucket page (through
 * the buffer pool). That is why the index stays fast as it grows.
 *
 * Bucket page (4096 bytes): u64 page_lsn, u16 local_depth, u16 count,
 * u32 crc32, then up to 78 entries of 52 bytes:
 *   hash[32], u32 container_id, u32 offset, u32 len, u32 refcount,
 *   u32 created_at
 *
 * dir.bin: "NXDR", u16 version, u16 0, u32 global_depth,
 *          2^global_depth x u32 page_id, u32 crc32 (over everything before),
 *          zero padding to a multiple of 4096 so the checkpoint can write it
 *          as pages through the doublewrite file.
 *
 * Locking: the caller holds the directory lock (SPEC §9 #3), shared for
 * lookup/insert/update/remove and exclusive for split. Page bytes are
 * protected by frame latches inside this module.
 */
#ifndef NX_EXTHASH_H
#define NX_EXTHASH_H

#include "engine/bufpool.h"

#include <stdbool.h>
#include <stdint.h>

#define NX_BUCKET_HEADER 16u
#define NX_INDEX_ENTRY_SIZE 52u
#define NX_BUCKET_CAPACITY ((NX_PAGE_SIZE - NX_BUCKET_HEADER) / NX_INDEX_ENTRY_SIZE) /* 78 */
#define NX_EXT_INITIAL_DEPTH 4u
#define NX_EXT_MAX_DEPTH 24u

/* Return codes besides 0 and -1. */
#define NX_EXT_NOT_FOUND 1
#define NX_EXT_EXISTS 2
#define NX_EXT_NEED_SPLIT 3 /* bucket full: take the exclusive lock, split, retry */
#define NX_EXT_SKIPPED 4    /* redo: the page already contains this change */
#define NX_EXT_UNDERFLOW 5  /* refcount would go below zero */

typedef struct {
    uint8_t hash[32];
    uint32_t container_id;
    uint32_t offset;
    uint32_t len;
    uint32_t refcount;
    uint32_t created_at;
} nx_index_entry;

typedef struct nx_exthash nx_exthash;

/* dir_image: contents of dir.bin, or NULL/0 for a brand-new index (one
 * empty bucket, global depth 4). The bucket file is file_id in the pool. */
int nx_exthash_open(nx_bufpool *bp, uint32_t file_id, const uint8_t *dir_image, size_t dir_len,
                    nx_exthash **out);
void nx_exthash_close(nx_exthash *ex);

void nx_exthash_lock_shared(nx_exthash *ex);
void nx_exthash_lock_exclusive(nx_exthash *ex);
void nx_exthash_unlock(nx_exthash *ex);

/* All take `lsn` (the WAL record being applied). With redo=true a change is
 * skipped (NX_EXT_SKIPPED) if the bucket's page_lsn >= lsn. Every function
 * may also return NX_BP_FULL (run a checkpoint, retry) or NX_BP_CORRUPT. */
int nx_exthash_lookup(nx_exthash *ex, const uint8_t hash[32], nx_index_entry *out);
int nx_exthash_insert(nx_exthash *ex, const nx_index_entry *e, uint64_t lsn, bool redo);
/* Adds delta to the refcount; *new_ref receives the result if not NULL. */
int nx_exthash_add_ref(nx_exthash *ex, const uint8_t hash[32], int64_t delta, uint64_t lsn,
                       bool redo, uint32_t *new_ref);
int nx_exthash_set_location(nx_exthash *ex, const uint8_t hash[32], uint32_t container_id,
                            uint32_t offset, uint64_t lsn, bool redo);
int nx_exthash_remove(nx_exthash *ex, const uint8_t hash[32], uint64_t lsn, bool redo);

/* Splits the bucket that `hash` maps to (doubling the directory first if
 * needed). Caller holds the exclusive lock. */
int nx_exthash_split(nx_exthash *ex, const uint8_t hash[32], uint64_t lsn);

/* Visits every entry (bucket pages in page-id order). Caller holds at
 * least the shared lock. Stops if cb returns non-zero. */
typedef int (*nx_index_cb)(const nx_index_entry *e, void *ctx);
int nx_exthash_for_each(nx_exthash *ex, nx_index_cb cb, void *ctx);

/* Serialized dir.bin (malloc'd, length a multiple of 4096). */
int nx_exthash_dir_image(nx_exthash *ex, uint8_t **buf, size_t *len);
/* True when the directory changed since the last call with clear=true. */
bool nx_exthash_dir_dirty(nx_exthash *ex, bool clear);

typedef struct {
    uint64_t entries;
    uint32_t global_depth;
    uint32_t buckets;
    double avg_fill_percent;
} nx_exthash_stats;

void nx_exthash_get_stats(nx_exthash *ex, nx_exthash_stats *out);
uint32_t nx_exthash_bucket_count(nx_exthash *ex);

/* Directory slot a hash maps to at a given depth (exposed for tests). */
uint32_t nx_exthash_slot(const uint8_t hash[32], uint32_t depth);

#endif
