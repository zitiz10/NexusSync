/*
 * engine_internal.h — the engine's state and the few internal helpers that
 * the maintenance code (gc.c: cleanup and compaction; scan.c: integrity
 * scan and --rebuild-index) shares with engine.c. Nothing outside
 * src/engine/ includes this file; the rest of the server uses engine.h.
 */
#ifndef NX_ENGINE_INTERNAL_H
#define NX_ENGINE_INTERNAL_H

#include "engine.h"
#include "engine/container.h"
#include "engine/logrec.h"
#include "engine/superblock.h"
#include "engine/wal.h"
#include "platform/platform.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

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
    /* --rebuild-index: log replay skips index records (the index is being
     * rebuilt from the containers, D-048). Set only inside open. */
    bool rebuilding;
    nx_engine_recovery rinfo;

    /* Maintenance (M12, SPEC §9): one cleanup or scan at a time. */
    pthread_mutex_t maint_mu;

    /* Store barrier (D-048). A store registers in gate_n[epoch & 1] from
     * its first container append until its CHUNK_ADD has committed;
     * compaction bumps the epoch and waits for the old slot to drain. */
    pthread_mutex_t gate_mu;
    pthread_cond_t gate_cv;
    uint64_t gate_epoch;
    uint64_t gate_n[2];

    _Atomic uint64_t next_file_id;
    _Atomic uint64_t last_lsn; /* copy of stable_lsn for readers of stats */
    _Atomic uint64_t logical, physical, unique_blocks, saved;
    _Atomic uint64_t checkpoints, last_ckpt_pages, last_ckpt_ms, ckpt_lsn;
    _Atomic uint64_t last_ckpt_at; /* unix ms of the last checkpoint this run (0: none yet) */
};

/* Index lookup with the split / pool-full retries. have_txn: the caller
 * holds txn_mu. Returns 0, NX_EXT_NOT_FOUND, or -1. */
int nxi_lookup(nx_engine *e, const uint8_t hash[32], nx_index_entry *out, bool have_txn);

/* Unlogged index changes for --rebuild-index (caller holds txn_mu; pages
 * are stamped with stable_lsn and made durable by the checkpoint that
 * follows). Return the exthash codes (0, NX_EXT_EXISTS, NX_EXT_NOT_FOUND). */
int nxi_index_insert(nx_engine *e, const nx_index_entry *ent);
int nxi_index_add_ref(nx_engine *e, const uint8_t hash[32], uint32_t count);

/* Logs and applies one transaction (caller holds txn_mu). */
int nxi_log_and_apply(nx_engine *e, const nx_txn *t);

/* SPEC §8 checkpoint at stable_lsn; caller holds txn_mu. */
int nxi_checkpoint_locked(nx_engine *e);

/* Visits every index entry under the shared directory lock. If the pool
 * is full the walk is restarted after a checkpoint, so reset (if given)
 * is called first to forget what the partial walk collected. */
int nxi_walk_index(nx_engine *e, nx_index_cb cb, void *ctx, void (*reset)(void *ctx));

/* Store barrier (D-048). */
uint64_t nxi_gate_enter(nx_engine *e);
void nxi_gate_leave(nx_engine *e, uint64_t epoch);
/* Returns once every store that entered before this call has left. */
void nxi_gate_barrier(nx_engine *e);

/* Opens the engine; with rebuild, log replay skips index records. */
int nxi_open(const nx_engine_config *cfg, bool rebuild, nx_engine **out);

/* Recounts the physical-size statistics from the index (after a rebuild). */
int nxi_recount_blocks(nx_engine *e);

#endif
