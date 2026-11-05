/*
 * wal.h — the write-ahead log (SPEC §5, §8).
 *
 * data/wal/wal.NNNNNNNN segment files, about 64 MB each:
 *   header (16 bytes): "NXWL", u16 version, u16 0, u64 first_lsn
 *   records: u64 lsn, u64 txn_id, u8 type, u32 payload_len, payload,
 *            u32 crc32 over all preceding bytes of the record
 *
 * Rules that make recovery simple:
 *   - LSNs are consecutive (each record's LSN is the previous one + 1).
 *   - A transaction (BEGIN, records, COMMIT) is written with one write and
 *     never split across segments, then fsynced: that fsync is the commit
 *     point.
 *   - A segment is fsynced before the next one is started, so only the
 *     last segment can have a torn tail. A bad record anywhere else is
 *     real damage, and the WAL refuses to open rather than silently drop
 *     committed transactions.
 *
 * Recovery reads every record after the checkpoint LSN, then cuts the log
 * back to the end of the last COMMIT (or CHECKPOINT) record: a torn tail
 * or the records of a transaction that never committed are removed, and
 * their LSNs are handed out again.
 *
 * Locking: the WAL mutex (SPEC §9 #7). The engine calls it under the
 * transaction mutex anyway.
 */
#ifndef NX_WAL_H
#define NX_WAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NX_WAL_SEG_HEADER 16u
#define NX_WAL_REC_OVERHEAD 25u /* lsn 8, txn 8, type 1, len 4, crc 4 */
#define NX_WAL_MAX_PAYLOAD 4096u
#define NX_WAL_VERSION 1u
#define NX_WAL_DEFAULT_SEGMENT (64ull << 20)

typedef struct {
    uint64_t lsn;
    uint64_t txn_id;
    uint8_t type;
    uint32_t len;
    const uint8_t *payload;
} nx_wal_rec;

typedef struct nx_wal nx_wal;

/* Opens dir (creating it and a first segment starting at first_lsn if it
 * holds none). Call nx_wal_recover before appending. */
int nx_wal_open(const char *dir, uint64_t segment_size, uint64_t first_lsn, nx_wal **out);
void nx_wal_close(nx_wal *w);

typedef int (*nx_wal_cb)(const nx_wal_rec *r, void *ctx);

typedef struct {
    uint64_t records_read;      /* valid records with lsn > from_lsn */
    uint64_t bytes_cut;         /* torn or uncommitted tail removed */
    uint64_t last_lsn;          /* last record kept (0 if none) */
} nx_wal_recovery;

/* Calls cb for every valid record with lsn > from_lsn, in order (including
 * records of a final transaction without COMMIT: the caller must only
 * apply transactions it sees committed). Then cuts the log after the last
 * COMMIT or CHECKPOINT record. Returns -1 if the log is damaged anywhere
 * but the tail, or if cb fails. */
int nx_wal_recover(nx_wal *w, uint64_t from_lsn, nx_wal_cb cb, void *ctx, nx_wal_recovery *out);

/* Appends records (their LSNs must continue the log: next_lsn, +1, ...)
 * with one write, and fsyncs when sync is true. After any failure the WAL
 * refuses further appends. */
int nx_wal_append(nx_wal *w, const nx_wal_rec *recs, size_t n, bool sync);

/* LSN the next appended record must carry. */
uint64_t nx_wal_next_lsn(nx_wal *w);

/* Deletes segments that hold only records <= lsn (never the active one). */
int nx_wal_truncate_below(nx_wal *w, uint64_t lsn);

typedef struct {
    uint32_t segments;
    uint64_t bytes;
} nx_wal_stats;

void nx_wal_get_stats(nx_wal *w, nx_wal_stats *out);

#endif
