/*
 * stats.h — storage numbers and the block map (SPEC §12 "Stats", "Block map").
 *
 * GET /api/stats       global storage numbers for everyone, plus `mine` (the
 *                      caller's own files); engine internals for admins only.
 * GET /api/blockmap    the physical store cut into N equal tiles in storage
 *                      order, each with a share level 0–4; with ?file= the
 *                      tiles that hold that file's blocks.
 *
 * The block map needs every index entry sorted by (container, offset). That
 * walk is cached and rebuilt at most every 2 s after a change (the engine's
 * last LSN tells us something changed), so a busy lab cannot make the
 * server re-walk the index for every request (DECISIONS D-040).
 *
 * stats.updated: the background thread calls nx_stats_tick every 250 ms;
 * when the engine changed since the last event and 500 ms have passed, it
 * publishes the global numbers to everyone (at most 2 events per second).
 */
#ifndef NX_STATS_H
#define NX_STATS_H

#include "net/router.h"

#include <stddef.h>
#include <stdint.h>

#define NX_BLOCKMAP_TILES 384u
#define NX_BLOCKMAP_MAX_TILES 4096u

typedef struct nx_stats nx_stats;

int nx_stats_create(nx_stats **out);
void nx_stats_destroy(nx_stats *s);

/* Background thread: publishes stats.updated when due. */
void nx_stats_tick(nx_app *app);

/* Tiles (out of NX_BLOCKMAP_TILES) holding the newest `batch_bytes` of the
 * physical store, i.e. blocks just appended (they are last in storage
 * order). Writes at most cap indices to out; returns how many. */
size_t nx_stats_tail_tiles(nx_engine *e, uint64_t batch_bytes, uint32_t *out, size_t cap);

/* Share level of a byte-weighted mean share count (D-040):
 * 0 empty, 1 written once, 2 two files, 3 three or four, 4 five or more. */
int nx_share_level(double mean_refs);

void nx_h_stats(nx_ctx *c);
void nx_h_blockmap(nx_ctx *c);

#endif
