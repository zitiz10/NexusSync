/*
 * superblock.h — the root record of a data directory (SPEC §5).
 *
 * Two copies, superblock.a and superblock.b, written alternately: copy
 * number seq % 2 (odd seq -> .a, even seq -> .b). A crash while writing one
 * copy leaves the other intact, so there is always one valid superblock.
 * Loading picks the valid copy (magic, version, CRC) with the higher seq.
 *
 * Layout (4096 bytes, little-endian):
 *   [0]  "NXSB"            [4]  u16 version   [6] u16 0
 *   [8]  u64 seq           [16] u64 checkpoint_lsn
 *   [24] u64 next_file_id  [32] u32 next_user_id
 *   [36] u32 next_container_id   [40] u32 active_container_id
 *   [44] u32 index_global_depth  [48] u32 bucket_page_count
 *   [52] u32 cdc_min  [56] cdc_avg  [60] cdc_max  [64] mask_s  [68] mask_l
 *   [72] u32 gear_seed_lo  [76] u32 gear_seed_hi
 *   [80] u64 created_at    [88] u32 crc32 over bytes [0, 88)
 *   zero padding to 4096
 */
#ifndef NX_SUPERBLOCK_H
#define NX_SUPERBLOCK_H

#include <stdint.h>

#define NX_SUPERBLOCK_SIZE 4096u
#define NX_SUPERBLOCK_VERSION 1u
#define NX_GEAR_SEED 0x4E657875735379ull /* SPEC §6.2 */

typedef struct {
    uint64_t seq;
    uint64_t checkpoint_lsn;
    uint64_t next_file_id;
    uint32_t next_user_id;
    uint32_t next_container_id;
    uint32_t active_container_id;
    uint32_t index_global_depth;
    uint32_t bucket_page_count;
    uint32_t cdc_min, cdc_avg, cdc_max, mask_s, mask_l;
    uint32_t gear_seed_lo, gear_seed_hi;
    uint64_t created_at;
} nx_superblock;

/* Fills a superblock for a brand-new data directory (seq 0, ids from 1,
 * chunker parameters from chunk.h). */
void nx_superblock_init(nx_superblock *sb, uint64_t created_at);

void nx_superblock_encode(const nx_superblock *sb, uint8_t out[NX_SUPERBLOCK_SIZE]);
/* 0 if the page is a valid superblock, -1 otherwise. */
int nx_superblock_decode(const uint8_t in[NX_SUPERBLOCK_SIZE], nx_superblock *sb);

/* Loads the newest valid copy from data_dir. Returns 0, 1 if neither copy
 * exists (not initialized), or -1 if copies exist but none is valid. */
int nx_superblock_load(const char *data_dir, nx_superblock *out);

/* Increments sb->seq and writes it to the copy that seq selects; fsyncs the
 * file (and the directory when the file is new). */
int nx_superblock_write(const char *data_dir, nx_superblock *sb);

/* True when the chunker parameters match this build (SPEC §6.2: changing
 * them is a format change, so a mismatching data dir must not be opened). */
int nx_superblock_params_match(const nx_superblock *sb);

#endif
