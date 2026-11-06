/*
 * compare.h — GET /api/compare (SPEC §12 "Compare", DECISIONS D-043).
 *
 * Shows how version 2 of a file (b) relates to version 1 (a) when both are
 * cut into fixed 4 KB blocks or content-defined blocks: which blocks of b
 * already exist in a, the first difference, a window of blocks with their
 * hashes and storage locations, and a run-length minimap of the whole file.
 *
 * Results are cached per (a, b, mode) for 5 minutes (4 entries): fixed mode
 * reads both files in full, and the screen asks again on every window move.
 */
#ifndef NX_COMPARE_H
#define NX_COMPARE_H

#include "net/router.h"

#define NX_COMPARE_MAX_FILE (1ull << 30) /* 1 GiB per side */
#define NX_COMPARE_MAX_WINDOW 64u
#define NX_COMPARE_CACHE 4u
#define NX_COMPARE_TTL_MS (5u * 60u * 1000u)

typedef struct nx_compare nx_compare;

int nx_compare_create(nx_compare **out);
void nx_compare_destroy(nx_compare *c);

void nx_h_compare(nx_ctx *c);

#endif
