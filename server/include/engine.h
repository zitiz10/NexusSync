/*
 * engine.h — the storage engine's API (SPEC §7.5). The HTTP layer talks to
 * storage only through these functions.
 *
 * Every change is a transaction of WAL records (logrec.h) that is logged
 * and then applied to pages by one apply function, the same one recovery
 * uses. M4 hands out LSNs without writing a log; M5 adds the WAL file, its
 * fsync (the commit point), and recovery.
 *
 * Thread safety: every function may be called from any thread.
 */
#ifndef NX_ENGINE_H
#define NX_ENGINE_H

#include "engine/bufpool.h"
#include "engine/catalog.h"
#include "engine/exthash.h"
#include "engine/recipe.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Results besides 0 (ok) and -1 (I/O or memory failure). */
#define NX_ENGINE_NOT_FOUND 1 /* no such file or user */
#define NX_ENGINE_MISSING 2   /* commit: some chunks are not on the server (HTTP 409) */
#define NX_ENGINE_BAD_HASH 3  /* data does not match the hash it was sent with (HTTP 422) */
#define NX_ENGINE_INVALID 4   /* malformed request (sizes, lengths, names) */
#define NX_ENGINE_EXISTS 5    /* username already taken */
#define NX_ENGINE_CORRUPT 6   /* stored data failed verification */
#define NX_ENGINE_STOPPED 7   /* the read callback asked to stop */

typedef struct {
    const char *data_dir;
    uint64_t container_size; /* SPEC §3 container_size */
    size_t pool_pages;       /* buffer_pool_pages */
    bool verify_on_read;
    bool verify_on_commit;
    uint64_t wal_segment_size; /* bytes per WAL segment (64 MB) */
    /* Optional: counts log records redone while nx_engine_open replays, so
     * /api/health can show live progress (D-046). NULL to skip. */
    _Atomic uint64_t *replay_progress;
} nx_engine_config;

void nx_engine_config_defaults(nx_engine_config *cfg);

typedef struct nx_engine nx_engine;

/* --init: creates an empty data directory. Fails if one already exists. */
int nx_engine_create(const char *data_dir);
int nx_engine_open(const nx_engine_config *cfg, nx_engine **out);
/* Runs a final checkpoint, then frees everything. */
int nx_engine_close(nx_engine *e);
/* Tests only: frees everything without writing anything, as if the process
 * had been killed at this moment. The next open runs recovery. */
void nx_engine_abandon(nx_engine *e);

/* What the last open had to do to recover (SPEC §8 "Recovery"). */
typedef struct {
    uint64_t pages_repaired;   /* copied in place from doublewrite.bin */
    uint64_t records_replayed; /* records of committed transactions redone */
    uint64_t txns_redone;
    uint64_t txns_discarded;   /* found in the log without COMMIT */
    uint64_t wal_bytes_cut;    /* torn or uncommitted tail removed */
    uint64_t checkpoint_lsn;   /* where replay started */
    uint64_t ms;
} nx_engine_recovery;

void nx_engine_last_recovery(nx_engine *e, nx_engine_recovery *out);

/* ---- blocks ------------------------------------------------------------- */

/* 0 and *out filled, or NX_ENGINE_NOT_FOUND. */
int nx_engine_lookup(nx_engine *e, const uint8_t hash[32], nx_index_entry *out);

typedef struct {
    const uint8_t *data;
    uint32_t len;
    uint8_t hash[32]; /* what the client claims; checked, never trusted */
} nx_block_in;

typedef struct {
    size_t stored;          /* new blocks written */
    size_t duplicate;       /* already on the server (or twice in the batch) */
    uint64_t bytes_written; /* data bytes of the stored blocks */
} nx_store_result;

/* Hashes every block (NX_ENGINE_BAD_HASH if one does not match: nothing is
 * stored), appends the new ones to the active container, fsyncs it, then
 * commits one CHUNK_ADD transaction (refcount 0) for the batch. */
int nx_engine_store_blocks(nx_engine *e, const nx_block_in *blocks, size_t n,
                           nx_store_result *res);

/* ---- files -------------------------------------------------------------- */

typedef struct {
    uint32_t owner_id;
    const char *name; /* UTF-8, 1..255 bytes */
    uint8_t chunker;  /* enum nx_chunker_kind */
    uint64_t size;
    uint8_t sha256[32];  /* whole-file hash the client computed */
    uint64_t bytes_sent; /* bytes that crossed the network for this upload */
    /* Which file this is a new version of (SPEC §12 versionOf):
     *   0               the newest file of this owner with this name, if any
     *   NX_VERSION_NONE a new file (version 1), even if the name exists
     *   other           that file id (same owner), else NX_ENGINE_NOT_FOUND */
    uint64_t version_of;
} nx_commit_meta;

#define NX_VERSION_NONE UINT64_MAX

typedef struct {
    uint64_t file_id;
    uint32_t version;
    /* On NX_ENGINE_MISSING: the distinct hashes that are not stored
     * (malloc'd; free with nx_commit_result_free). */
    uint8_t (*missing)[32];
    size_t missing_count;
} nx_commit_result;

void nx_commit_result_free(nx_commit_result *r);

/* Writes the recipe (fsynced), checks every chunk exists, optionally
 * re-reads the whole file and checks its SHA-256, then commits REF_INC per
 * distinct block and FILE_ADD. Same owner + name -> next version. */
int nx_engine_commit_file(nx_engine *e, const nx_commit_meta *meta, const nx_chunk_ref *chunks,
                          uint32_t count, nx_commit_result *res);

/* FILE_DEL + REF_DEC per distinct block. */
int nx_engine_delete_file(nx_engine *e, uint64_t file_id);

/* FILE_UPDATE: sets the flags byte (NX_FILE_SHARED, NX_FILE_DELETED). */
int nx_engine_set_file_flags(nx_engine *e, uint64_t file_id, uint8_t flags);

/* Streams the file's bytes to cb in order. Each block is checked against
 * its hash (verify_on_read) and the whole file against its SHA-256; on a
 * mismatch NX_ENGINE_CORRUPT is returned (bytes already passed to cb must
 * be discarded by the caller). cb returns non-zero to stop. */
typedef int (*nx_engine_write_cb)(const uint8_t *data, size_t len, void *ctx);
int nx_engine_read_file(nx_engine *e, uint64_t file_id, nx_engine_write_cb cb, void *ctx);

/* The recipe of a committed file (malloc'd; caller frees *chunks). */
int nx_engine_file_recipe(nx_engine *e, uint64_t file_id, nx_file *meta, nx_chunk_ref **chunks);

/* ---- users ---------------------------------------------------------------- */

/* Assigns u->id. NX_ENGINE_EXISTS if the username is taken. */
int nx_engine_add_user(nx_engine *e, nx_user *u);
/* Replaces an existing user's record (same id). */
int nx_engine_update_user(nx_engine *e, const nx_user *u);

/* Read-only access for queries (catalog functions are thread-safe). */
nx_catalog *nx_engine_catalog(nx_engine *e);

/* ---- maintenance and stats ------------------------------------------------- */

int nx_engine_checkpoint(nx_engine *e);

typedef struct {
    uint64_t files_checked;
    uint64_t chunks_checked;  /* chunk references read and verified */
    uint64_t blocks_checked;  /* index entries compared */
    uint64_t i1_errors;       /* referenced block missing or damaged */
    uint64_t i2_errors;       /* refcount differs from the recipes */
    uint64_t i3_errors;       /* whole-file hash or size mismatch */
    uint64_t other_errors;    /* unreadable recipe, I/O */
    char first_error[256];
} nx_fsck_report;

/* Checks invariants I1–I3 (SPEC §1) over everything committed. Returns 0
 * when all hold, 1 when something is wrong (see report), -1 on failure. */
int nx_engine_fsck(nx_engine *e, nx_fsck_report *r);

typedef struct {
    uint64_t files;
    uint64_t users;
    uint64_t logical_bytes;   /* sum of file sizes */
    uint64_t physical_bytes;  /* sum of stored block lengths */
    uint64_t unique_blocks;   /* index entries */
    uint64_t writes_skipped_bytes;  /* logical - physical (0 if negative) */
    uint64_t network_saved_bytes;   /* sum of (size - bytes_sent) */
    uint64_t last_lsn;
    uint64_t checkpoint_lsn;
    uint64_t checkpoints;
    uint64_t last_checkpoint_pages;
    uint64_t last_checkpoint_ms;
    uint64_t last_checkpoint_at_ms; /* unix ms; 0 if no checkpoint ran since open */
    uint64_t wal_bytes;
    uint32_t wal_segments;
    nx_bufpool_stats pool;
    nx_exthash_stats index;
} nx_engine_stats;

void nx_engine_get_stats(nx_engine *e, nx_engine_stats *out);

/* Where one stored block lives (an index entry without its hash). */
typedef struct {
    uint32_t container_id;
    uint32_t offset; /* of the record header in the container */
    uint32_t len;    /* data bytes */
    uint32_t refcount;
} nx_block_span;

/* Every index entry in storage order (container id, then offset): the
 * order the block map (SPEC §12) slices the physical store in. malloc'd. */
int nx_engine_block_spans(nx_engine *e, nx_block_span **out, size_t *count);

typedef struct {
    uint32_t id;
    uint64_t file_bytes; /* including the 64-byte header */
} nx_container_info;

/* Every container file with its size, ascending ids. malloc'd. */
int nx_engine_containers(nx_engine *e, nx_container_info **out, size_t *count);

#endif
