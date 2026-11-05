/*
 * hmap.h — small open-addressing hash maps used in memory only:
 *   nx_hmap32: 32-byte key (a block hash) -> u64 value   (fsck, gc, uploads)
 *   nx_hmap64: u64 key (a file or user id) -> u64 value  (catalog indexes)
 * Linear probing, power-of-two capacity, grows at 70% load. Not
 * thread-safe: owners protect them with their own locks.
 */
#ifndef NX_HMAP_H
#define NX_HMAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *keys;   /* cap * 32 */
    uint64_t *vals;  /* cap */
    uint8_t *used;   /* cap: 0 empty, 1 used, 2 deleted */
    size_t cap, count, tombstones;
} nx_hmap32;

int nx_hmap32_init(nx_hmap32 *m, size_t expected);
void nx_hmap32_free(nx_hmap32 *m);
/* Returns a pointer to the value slot, inserting with value 0 if absent
 * (sets *inserted). NULL on out-of-memory. */
uint64_t *nx_hmap32_upsert(nx_hmap32 *m, const uint8_t key[32], bool *inserted);
uint64_t *nx_hmap32_get(const nx_hmap32 *m, const uint8_t key[32]);
bool nx_hmap32_remove(nx_hmap32 *m, const uint8_t key[32]);
/* Iteration: for (size_t i = 0; i < m->cap; i++) if (m->used[i] == 1) ... */

typedef struct {
    uint64_t *keys;
    uint64_t *vals;
    uint8_t *used;
    size_t cap, count, tombstones;
} nx_hmap64;

int nx_hmap64_init(nx_hmap64 *m, size_t expected);
void nx_hmap64_free(nx_hmap64 *m);
int nx_hmap64_put(nx_hmap64 *m, uint64_t key, uint64_t val);
bool nx_hmap64_get(const nx_hmap64 *m, uint64_t key, uint64_t *val);
bool nx_hmap64_remove(nx_hmap64 *m, uint64_t key);

#endif
