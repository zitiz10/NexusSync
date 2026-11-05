/*
 * chunk.h — splitting a byte stream into blocks (SPEC §6).
 *
 * Two methods:
 *   fixed: every block is 4096 bytes; the last holds the remainder.
 *   cdc:   content-defined, FastCDC style. A rolling Gear hash over the
 *          bytes decides where blocks end, so an insertion only moves the
 *          boundaries near it (SPEC §6.2).
 *
 * The parameters below are part of the on-disk format and of the browser
 * protocol (the TS chunker must match byte for byte). Changing any of them
 * is a format version bump.
 */
#ifndef NX_CHUNK_H
#define NX_CHUNK_H

#include <stddef.h>
#include <stdint.h>

enum nx_chunker_kind { NX_CHUNKER_FIXED = 0, NX_CHUNKER_CDC = 1 };

#define NX_FIXED_SIZE 4096u

#define NX_CDC_MIN 2048u
#define NX_CDC_AVG 4096u
#define NX_CDC_MAX 16384u
#define NX_CDC_MASK_S 0xFFFC0000u /* 14 bits set: stricter, used before AVG */
#define NX_CDC_MASK_L 0xFFC00000u /* 10 bits set: looser, used after AVG */

/* Largest block either method can produce. */
#define NX_CHUNK_MAX_SIZE NX_CDC_MAX

/* Length of the first block of buf[0..n). Pure functions: given the same
 * n bytes they always return the same length. n == 0 returns 0. */
size_t nx_cdc_cut(const uint8_t *buf, size_t n);
size_t nx_fixed_cut(const uint8_t *buf, size_t n);

/* Called once per block, in order. Return non-zero to stop chunking; that
 * value is then returned by push/finish. */
typedef int (*nx_chunk_cb)(const uint8_t *data, size_t len, void *ctx);

/*
 * Streaming chunker: feed any number of pieces of any size, then finish.
 * Produces exactly the same blocks as cutting the whole file in memory,
 * because a cut never looks more than NX_CHUNK_MAX_SIZE bytes ahead and we
 * only cut once that many bytes are buffered (or the input has ended).
 */
typedef struct {
    enum nx_chunker_kind kind;
    size_t len;
    uint8_t buf[NX_CHUNK_MAX_SIZE];
} nx_chunker;

void nx_chunker_init(nx_chunker *c, enum nx_chunker_kind kind);
int nx_chunker_push(nx_chunker *c, const void *data, size_t len, nx_chunk_cb cb, void *ctx);
int nx_chunker_finish(nx_chunker *c, nx_chunk_cb cb, void *ctx);

#endif
