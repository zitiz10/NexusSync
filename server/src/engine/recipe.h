/*
 * recipe.h — file recipes: the ordered list of block fingerprints that
 * rebuilds a file (SPEC §5, §7.4).
 *
 * data/recipes/recipes.dat is append-only:
 *   file header (16 bytes): magic "NXRF", u16 version, u16 0, 8 zero bytes
 *   records: magic "NXRC", u64 file_id, u32 chunk_count,
 *            chunk_count x (u8 hash[32], u32 len), u32 crc32 (over the
 *            record's preceding bytes)
 *
 * A record is appended and fsynced *before* the WAL commit record that
 * points at it, so a committed file always has its recipe on disk. A crash
 * can leave a torn record at the tail; that is harmless, because recipes
 * are only ever reached through the offset stored in a committed file
 * record. Reads check magic, file id, count, and CRC.
 *
 * Locking: one leaf mutex for appends (SPEC §9). Reads use positional I/O
 * and need no lock.
 */
#ifndef NX_RECIPE_H
#define NX_RECIPE_H

#include <stdint.h>

#define NX_RECIPE_FILE_HEADER 16u
#define NX_RECIPE_VERSION 1u
#define NX_RECIPE_CHUNK_BYTES 36u /* hash 32 + len 4 */
#define NX_RECIPE_CORRUPT (-3)

typedef struct {
    uint8_t hash[32];
    uint32_t len;
} nx_chunk_ref;

typedef struct nx_recipes nx_recipes;

/* Opens (creating if missing) the recipe file. */
int nx_recipes_open(const char *path, nx_recipes **out);
void nx_recipes_close(nx_recipes *r);

/* Bytes one record takes for count chunks (0 if it would not fit a u32). */
uint32_t nx_recipe_record_len(uint32_t count);

/* Appends one record and fsyncs it. Returns its offset and length. */
int nx_recipes_append(nx_recipes *r, uint64_t file_id, const nx_chunk_ref *chunks, uint32_t count,
                      uint64_t *offset, uint32_t *len);

/* Reads the record at offset/len, checks that it is intact and belongs to
 * file_id with expect_count chunks. *chunks is malloc'd (caller frees).
 * Returns 0, NX_RECIPE_CORRUPT, or -1 on I/O or memory failure. */
int nx_recipes_read(nx_recipes *r, uint64_t offset, uint32_t len, uint64_t file_id,
                    uint32_t expect_count, nx_chunk_ref **chunks);

/* Current end of the file (where the next record goes). */
uint64_t nx_recipes_size(nx_recipes *r);

#endif
