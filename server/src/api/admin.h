/*
 * admin.h — the admin API (SPEC §12 "Admin"): users, the event log,
 * recovery history, LAN addresses; cleanup and scan arrive in M12.
 *
 * Recovery history lives in data/recoveries (DECISIONS D-046):
 *   header (8 bytes):  "NXRV", u16 version 1, u16 0
 *   records (64 bytes): u64 at_ms, u64 records_replayed, u64 txns_redone,
 *                       u64 txns_discarded, u64 pages_repaired,
 *                       u64 wal_bytes_cut, u64 ms, u32 0,
 *                       u32 crc32 (over the 60 bytes before it)
 * One record per start, appended and fsynced. A torn last record fails its
 * CRC and is skipped.
 */
#ifndef NX_ADMIN_H
#define NX_ADMIN_H

#include "net/router.h"

#define NX_RECOVERY_REC 64u
#define NX_RECOVERY_SHOWN 50u

typedef struct {
    uint64_t at_ms;
    nx_engine_recovery r;
} nx_recovery_entry;

/* Appends one start to data/recoveries (creating it). 0 or -1. */
int nx_recovery_log_append(const char *data_dir, uint64_t at_ms, const nx_engine_recovery *r);
/* Reads the newest `max` valid entries, newest first. Returns the count
 * (0 if the file does not exist), or -1. */
int nx_recovery_log_read(const char *data_dir, nx_recovery_entry *out, size_t max);

void nx_h_admin_users(nx_ctx *c);
void nx_h_admin_user_update(nx_ctx *c);
void nx_h_admin_events(nx_ctx *c);
void nx_h_admin_recoveries(nx_ctx *c);
void nx_h_admin_addresses(nx_ctx *c);
void nx_h_admin_gc(nx_ctx *c);
void nx_h_admin_scan(nx_ctx *c);

#endif
