/*
 * nx_engine_util.h — client-side helpers shared by the engine tests: play
 * the uploader (chunk, send only missing blocks, commit), read files back,
 * run fsck, and generate CSV-like content and small edits of it.
 */
#ifndef NX_ENGINE_UTIL_H
#define NX_ENGINE_UTIL_H

#include "crypto/sha256.h"
#include "engine.h"
#include "engine/chunk.h"
#include "nx_test.h"
#include "nx_test_util.h"


static inline void cfg_for(const char *dir, size_t pool, nx_engine_config *cfg)
{
    nx_engine_config_defaults(cfg);
    cfg->data_dir = dir;
    cfg->pool_pages = pool;
    cfg->container_size = 256 * 1024;
}

static inline nx_engine *open_fresh(const char *name, char *dir, size_t dir_len, size_t pool)
{
    nx_test_tmpdir(name, dir, dir_len);
    if (nx_engine_create(dir) != 0)
        return NULL;
    nx_engine_config cfg;
    cfg_for(dir, pool, &cfg);
    nx_engine *e = NULL;
    return nx_engine_open(&cfg, &e) == 0 ? e : NULL;
}

typedef struct {
    nx_chunk_ref *refs;
    uint32_t n;
    uint64_t sent; /* bytes of blocks that had to be sent */
    size_t stored;
} upload_info;

/* Plays the client: chunk, ask which blocks are missing, send only those
 * (in batches of 64), then commit. */
static inline int upload(nx_engine *e, uint32_t owner, const char *name, const uint8_t *data, size_t len,
                  uint8_t chunker, nx_commit_result *res, upload_info *info)
{
    memset(info, 0, sizeof *info);
    size_t cap = len / 1024 + 2;
    info->refs = malloc(cap * sizeof *info->refs);
    size_t *offs = malloc(cap * sizeof *offs);
    nx_block_in *batch = malloc(64 * sizeof *batch);
    if (!info->refs || !offs || !batch)
        return -1;
    for (size_t pos = 0; pos < len;) {
        size_t n = chunker == NX_CHUNKER_CDC ? nx_cdc_cut(data + pos, len - pos)
                                             : nx_fixed_cut(data + pos, len - pos);
        nx_sha256(data + pos, n, info->refs[info->n].hash);
        info->refs[info->n].len = (uint32_t)n;
        offs[info->n++] = pos;
        pos += n;
    }
    int rc = 0;
    size_t nb = 0;
    for (uint32_t i = 0; i <= info->n && rc == 0; i++) {
        if (i < info->n) {
            nx_index_entry ent;
            if (nx_engine_lookup(e, info->refs[i].hash, &ent) == NX_ENGINE_NOT_FOUND) {
                batch[nb].data = data + offs[i];
                batch[nb].len = info->refs[i].len;
                memcpy(batch[nb].hash, info->refs[i].hash, 32);
                info->sent += info->refs[i].len;
                nb++;
            }
        }
        if (nb == 64 || (i == info->n && nb > 0)) {
            nx_store_result sr;
            rc = nx_engine_store_blocks(e, batch, nb, &sr);
            info->stored += sr.stored;
            nb = 0;
        }
    }
    if (rc == 0) {
        nx_commit_meta m = {.owner_id = owner,
                            .name = name,
                            .chunker = chunker,
                            .size = len,
                            .bytes_sent = info->sent};
        nx_sha256(data, len, m.sha256);
        rc = nx_engine_commit_file(e, &m, info->refs, info->n, res);
    }
    free(offs);
    free(batch);
    return rc;
}

typedef struct {
    uint8_t *buf;
    size_t len, cap;
} sink;

static inline int to_sink(const uint8_t *data, size_t len, void *ctx)
{
    sink *s = ctx;
    if (s->len + len > s->cap) {
        size_t cap = s->cap ? s->cap : 65536;
        while (cap < s->len + len)
            cap *= 2;
        uint8_t *b = realloc(s->buf, cap);
        if (!b)
            return 1;
        s->buf = b;
        s->cap = cap;
    }
    memcpy(s->buf + s->len, data, len);
    s->len += len;
    return 0;
}

static inline bool read_matches(nx_engine *e, uint64_t id, const uint8_t *want, size_t len)
{
    sink s = {0};
    int rc = nx_engine_read_file(e, id, to_sink, &s);
    bool ok = rc == 0 && s.len == len && (len == 0 || memcmp(s.buf, want, len) == 0);
    if (!ok)
        fprintf(stderr, "    read file %" PRIu64 ": rc=%d len=%zu want=%zu\n", id, rc, s.len, len);
    free(s.buf);
    return ok;
}

static inline void report_fsck(const nx_fsck_report *r)
{
    if (r->first_error[0])
        fprintf(stderr, "    fsck: %s (I1=%" PRIu64 " I2=%" PRIu64 " I3=%" PRIu64 " other=%" PRIu64
                        ")\n",
                r->first_error, r->i1_errors, r->i2_errors, r->i3_errors, r->other_errors);
}

static inline int fsck_ok(nx_engine *e, nx_fsck_report *r)
{
    int rc = nx_engine_fsck(e, r);
    report_fsck(r);
    return rc;
}

/* Text-like content: CSV rows built from a seed, so edits behave like real
 * edits of lab data. */
static inline uint8_t *make_content(nx_splitmix *rng, size_t len)
{
    uint8_t *b = malloc(len ? len : 1);
    if (!b)
        exit(2);
    size_t pos = 0;
    while (pos < len) {
        char row[96];
        int n = snprintf(row, sizeof row, "%" PRIu64 ",%" PRIu64 ",%.3f,sample-%" PRIu64 "\n",
                         nx_splitmix_next(rng) % 100000, nx_splitmix_next(rng) % 977,
                         (double)(nx_splitmix_next(rng) % 100000) / 7.0,
                         nx_splitmix_next(rng) % 50);
        size_t take = (size_t)n < len - pos ? (size_t)n : len - pos;
        memcpy(b + pos, row, take);
        pos += take;
    }
    return b;
}

/* An edited copy: insert, delete, or overwrite a few bytes somewhere. */
static inline uint8_t *edit_copy(nx_splitmix *rng, const uint8_t *src, size_t len, size_t *out_len)
{
    size_t kind = nx_splitmix_next(rng) % 3, at = len ? nx_splitmix_next(rng) % (len + 1) : 0;
    size_t k = 1 + nx_splitmix_next(rng) % 400;
    uint8_t *ins = make_content(rng, k);
    uint8_t *b = malloc(len + k + 1);
    if (!b)
        exit(2);
    if (kind == 0) { /* insert */
        memcpy(b, src, at);
        memcpy(b + at, ins, k);
        memcpy(b + at + k, src + at, len - at);
        *out_len = len + k;
    } else if (kind == 1) { /* delete */
        size_t d = at + k > len ? len - at : k;
        memcpy(b, src, at);
        memcpy(b + at, src + at + d, len - at - d);
        *out_len = len - d;
    } else { /* overwrite */
        memcpy(b, src, len);
        size_t w = at + k > len ? len - at : k;
        memcpy(b + at, ins, w);
        *out_len = len;
    }
    free(ins);
    return b;
}

#endif
