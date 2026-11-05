/*
 * chunk_fixtures.h — the 10 chunker fixtures (SPEC §6.4) and the format of
 * tests/fixtures/chunks_expected.txt, shared by test_chunk and chunkdump.
 *
 * chunks_expected.txt lines:
 *   file  <name> <size> <sha256 of file>
 *   cdc   <name> <count> <digest> <len,len,...|->
 *   fixed <name> <count> <digest>
 * digest = SHA-256 over the concatenated 32-byte hashes of every block, so
 * matching digests means every block hash matches, in order.
 */
#ifndef NX_CHUNK_FIXTURES_H
#define NX_CHUNK_FIXTURES_H

#include "crypto/sha256.h"
#include "engine/chunk.h"

#include <stdlib.h>
#include <string.h>

static const char *const NX_CHUNK_FIXTURES[] = {
    "f00_empty.bin",       "f01_1.bin",         "f02_2047.bin",
    "f03_2048.bin",        "f04_4096.bin",      "f05_16385.bin",
    "f06_1mb.bin",         "f07_10mb_random.bin", "f08_10mb_text.csv",
    "f09_10mb_text_insert.csv",
};
#define NX_CHUNK_FIXTURE_COUNT (sizeof NX_CHUNK_FIXTURES / sizeof NX_CHUNK_FIXTURES[0])

typedef struct {
    size_t count;
    size_t cap;
    uint32_t *lens;
    uint8_t (*hashes)[32];
    nx_sha256_ctx digest; /* over concatenated block hashes */
} nx_chunk_list;

static inline int nx_chunk_list_add(const uint8_t *data, size_t len, void *vctx)
{
    nx_chunk_list *l = vctx;
    if (l->count == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 256;
        uint32_t *lens = realloc(l->lens, cap * sizeof *lens);
        if (!lens)
            return -1;
        l->lens = lens;
        uint8_t(*h)[32] = realloc(l->hashes, cap * sizeof *h);
        if (!h)
            return -1;
        l->hashes = h;
        l->cap = cap;
    }
    l->lens[l->count] = (uint32_t)len;
    nx_sha256(data, len, l->hashes[l->count]);
    nx_sha256_update(&l->digest, l->hashes[l->count], 32);
    l->count++;
    return 0;
}

/* Chunks buf with the streaming chunker, feeding `piece` bytes at a time
 * (0 = all at once). Returns 0 or -1. Caller frees with nx_chunk_list_free. */
static inline int nx_chunk_all(enum nx_chunker_kind kind, const uint8_t *buf, size_t n,
                               size_t piece, nx_chunk_list *out, uint8_t digest[32])
{
    memset(out, 0, sizeof *out);
    nx_sha256_init(&out->digest);
    nx_chunker *c = malloc(sizeof *c);
    if (!c)
        return -1;
    nx_chunker_init(c, kind);
    size_t step = piece ? piece : (n ? n : 1);
    int rc = 0;
    for (size_t off = 0; off < n && rc == 0; off += step) {
        size_t take = n - off < step ? n - off : step;
        rc = nx_chunker_push(c, buf + off, take, nx_chunk_list_add, out);
    }
    if (rc == 0)
        rc = nx_chunker_finish(c, nx_chunk_list_add, out);
    free(c);
    nx_sha256_final(&out->digest, digest);
    return rc == 0 ? 0 : -1;
}

static inline void nx_chunk_list_free(nx_chunk_list *l)
{
    free(l->lens);
    free(l->hashes);
    memset(l, 0, sizeof *l);
}

#endif
