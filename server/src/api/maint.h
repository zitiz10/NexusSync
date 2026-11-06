/*
 * maint.h — maintenance jobs: cleanup and the integrity scan, started by an
 * admin (POST /api/admin/gc, /api/admin/scan) or by the nightly schedule
 * at 02:00 local time (SPEC §10, §12; DECISIONS D-048).
 *
 * A job runs on its own thread, never on an HTTP worker, and reports
 * through WebSocket events (engine.gc, engine.scan, engine.corruption).
 * One job at a time: a second request answers 409 maintenance_running.
 *
 * The last scan result is kept in data/scan_last (36 bytes: "NXSN", u16 1,
 * u16 0, u64 at_ms, u64 verified, u64 failures, u32 crc32 over the first
 * 32 bytes), replaced atomically, so the Engine screen still knows it
 * after a restart.
 */
#ifndef NX_MAINT_H
#define NX_MAINT_H

#include "net/router.h"
#include "util/json_out.h"

#include <stdint.h>

typedef struct nx_maint nx_maint;

#define NX_SCAN_LAST_LEN 36u

int nx_maint_create(const char *data_dir, nx_maint **out);
/* Waits for a running job to finish. */
void nx_maint_destroy(nx_maint *m);

enum nx_job { NX_JOB_GC = 1, NX_JOB_SCAN = 2, NX_JOB_NIGHTLY = 3 };
/* 0 started, 1 a job is already running, -1 could not start. */
int nx_maint_start(nx_app *app, enum nx_job job);
/* Background thread, every 250 ms: starts the nightly job when due. */
void nx_maint_tick(nx_app *app);
/* Moves the next nightly run to at_s (unix seconds); tests use it to
 * check the nightly path without waiting for 02:00. */
void nx_maint_schedule(nx_app *app, uint64_t at_s);
/* Waits until no job is running (tests). */
void nx_maint_wait(nx_app *app);

/* The `cleanup` and `integrity` members of /api/stats (admins). */
void nx_maint_json_cleanup(nx_app *app, nx_json *j, uint64_t pending_bytes);
void nx_maint_json_integrity(nx_app *app, nx_json *j);

/* data/scan_last (exposed for tests). 0, 1 if there is none, -1 if damaged. */
int nx_scan_last_write(const char *data_dir, uint64_t at_ms, uint64_t verified, uint64_t failures);
int nx_scan_last_read(const char *data_dir, uint64_t *at_ms, uint64_t *verified, uint64_t *failures);

void nx_h_admin_gc(nx_ctx *c);
void nx_h_admin_scan(nx_ctx *c);

#endif
