/*
 * test_engine.c — the engine API end to end, in process (SPEC §7.5, M4):
 *   - 200 random files including exact duplicates, edited copies, and new
 *     versions: put, get (byte-exact), delete, fsck (I1–I3) clean, reopen
 *   - deduplication: an identical upload sends and stores nothing
 *   - error paths: missing chunks (409), bad block or file hash (422),
 *     malformed requests
 *   - fsck detects a damaged block (I1), a wrong refcount (I2), and a file
 *     whose stored hash does not match its bytes (I3)
 *   - users: add, duplicate name, update, persistence
 *   - concurrent uploads of overlapping files, then fsck
 * Small buffer pools and containers are used on purpose, so that pool-full
 * checkpoints in the middle of applying a transaction and container
 * rotation happen often.
 */
#include "crypto/sha256.h"
#include "engine.h"
#include "engine/chunk.h"
#include "nx_test.h"
#include "nx_test_util.h"

#include <pthread.h>

/* ---- helpers ------------------------------------------------------------- */

static void cfg_for(const char *dir, size_t pool, nx_engine_config *cfg)
{
    nx_engine_config_defaults(cfg);
    cfg->data_dir = dir;
    cfg->pool_pages = pool;
    cfg->container_size = 256 * 1024;
}

static nx_engine *open_fresh(const char *name, char *dir, size_t dir_len, size_t pool)
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
static int upload(nx_engine *e, uint32_t owner, const char *name, const uint8_t *data, size_t len,
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

static int to_sink(const uint8_t *data, size_t len, void *ctx)
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

static bool read_matches(nx_engine *e, uint64_t id, const uint8_t *want, size_t len)
{
    sink s = {0};
    int rc = nx_engine_read_file(e, id, to_sink, &s);
    bool ok = rc == 0 && s.len == len && (len == 0 || memcmp(s.buf, want, len) == 0);
    if (!ok)
        fprintf(stderr, "    read file %" PRIu64 ": rc=%d len=%zu want=%zu\n", id, rc, s.len, len);
    free(s.buf);
    return ok;
}

static void report_fsck(const nx_fsck_report *r)
{
    if (r->first_error[0])
        fprintf(stderr, "    fsck: %s (I1=%" PRIu64 " I2=%" PRIu64 " I3=%" PRIu64 " other=%" PRIu64
                        ")\n",
                r->first_error, r->i1_errors, r->i2_errors, r->i3_errors, r->other_errors);
}

static int fsck_ok(nx_engine *e, nx_fsck_report *r)
{
    int rc = nx_engine_fsck(e, r);
    report_fsck(r);
    return rc;
}

/* Text-like content: CSV rows built from a seed, so edits behave like real
 * edits of lab data. */
static uint8_t *make_content(nx_splitmix *rng, size_t len)
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
static uint8_t *edit_copy(nx_splitmix *rng, const uint8_t *src, size_t len, size_t *out_len)
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

/* ---- M4 acceptance: 200 random files ---------------------------------------- */

typedef struct {
    uint64_t id;
    uint32_t owner;
    char name[64];
    uint8_t *data;
    size_t len;
    uint8_t chunker;
    bool live;
} model_file;

static void test_round_trip_200_files(void)
{
    char dir[600];
    nx_engine *e = open_fresh("eng_200", dir, sizeof dir, 48);
    REQUIRE(e);
    enum { N = 200 };
    static model_file m[N];
    memset(m, 0, sizeof m);
    nx_splitmix rng = {2026};
    size_t dup_uploads = 0, dup_stored = 0, edits = 0, versions = 0;
    uint64_t edit_sent = 0, edit_bytes = 0;
    for (int i = 0; i < N; i++) {
        unsigned kind = i < 10 ? 0 : (unsigned)(nx_splitmix_next(&rng) % 10);
        model_file *f = &m[i];
        f->owner = 1 + (uint32_t)(nx_splitmix_next(&rng) % 4);
        snprintf(f->name, sizeof f->name, "file-%03d.csv", i);
        const model_file *src = &m[nx_splitmix_next(&rng) % (i ? (uint64_t)i : 1)];
        if (kind <= 2 || i < 10) { /* new content, sometimes empty or tiny */
            size_t len = (size_t)(nx_splitmix_next(&rng) % 300000);
            if (i == 3)
                len = 0;
            if (i == 4)
                len = 1;
            f->data = make_content(&rng, len);
            f->len = len;
        } else if (kind <= 4) { /* exact duplicate under another name */
            f->len = src->len;
            f->data = malloc(src->len ? src->len : 1);
            memcpy(f->data, src->data, src->len);
            dup_uploads++;
        } else if (kind <= 8) { /* edited copy */
            f->data = edit_copy(&rng, src->data, src->len, &f->len);
            edits++;
        } else { /* new version of an existing file (same owner + name) */
            f->owner = src->owner;
            snprintf(f->name, sizeof f->name, "%s", src->name);
            f->data = edit_copy(&rng, src->data, src->len, &f->len);
            versions++;
        }
        /* Mostly CDC, some fixed-size; a duplicate is re-uploaded the way
         * its source was (same file, same client settings). */
        uint8_t chunker = (i % 7 == 0) ? NX_CHUNKER_FIXED : NX_CHUNKER_CDC;
        if (kind == 3 || kind == 4)
            chunker = src->chunker;
        f->chunker = chunker;
        nx_commit_result res;
        upload_info info;
        int rc = upload(e, f->owner, f->name, f->data, f->len, chunker, &res, &info);
        REQUIRE(rc == 0);
        f->id = res.file_id;
        f->live = true;
        if (kind == 3 || kind == 4) {
            dup_stored += info.stored;
            CHECK_EQ_U64(info.sent, 0); /* identical upload sends nothing */
        }
        if (kind >= 5 && kind <= 8 && chunker == NX_CHUNKER_CDC) {
            edit_sent += info.sent;
            edit_bytes += f->len;
        }
        free(info.refs);
        CHECK(read_matches(e, f->id, f->data, f->len));
    }
    CHECK(dup_uploads > 0 && dup_stored == 0);
    fprintf(stderr, "    200 files: %zu duplicates, %zu edited copies, %zu new versions\n",
            dup_uploads, edits, versions);
    fprintf(stderr, "    edited copies (CDC) sent %" PRIu64 " of %" PRIu64 " bytes (%.1f%%)\n",
            edit_sent, edit_bytes, edit_bytes ? 100.0 * (double)edit_sent / (double)edit_bytes : 0);
    CHECK(edit_sent * 4 < edit_bytes); /* small edits re-send only nearby blocks */

    /* Versions: the newest (owner, name) record points back at the previous. */
    for (int i = 0; i < N; i++) {
        nx_file latest;
        REQUIRE(nx_catalog_latest_version(nx_engine_catalog(e), m[i].owner, m[i].name, &latest));
        if (latest.version > 1) {
            nx_file prev;
            CHECK(nx_catalog_file_by_id(nx_engine_catalog(e), latest.prev_version_id, &prev));
            CHECK(prev.version == latest.version - 1 && strcmp(prev.name, latest.name) == 0);
        }
    }

    nx_fsck_report r;
    CHECK(fsck_ok(e, &r) == 0);
    CHECK_EQ_U64(r.files_checked, N);

    /* Delete about a third, in random order; the rest must be unaffected. */
    size_t deleted = 0;
    for (int k = 0; k < 70; k++) {
        model_file *f = &m[nx_splitmix_next(&rng) % N];
        int rc = nx_engine_delete_file(e, f->id);
        if (f->live) {
            CHECK(rc == 0);
            f->live = false;
            deleted++;
        } else {
            CHECK(rc == NX_ENGINE_NOT_FOUND);
        }
    }
    uint64_t logical = 0;
    size_t live = 0;
    for (int i = 0; i < N; i++) {
        if (m[i].live) {
            CHECK(read_matches(e, m[i].id, m[i].data, m[i].len));
            logical += m[i].len;
            live++;
        } else {
            CHECK(nx_engine_read_file(e, m[i].id, NULL, NULL) == NX_ENGINE_NOT_FOUND);
        }
    }
    CHECK(fsck_ok(e, &r) == 0);
    nx_engine_stats st;
    nx_engine_get_stats(e, &st);
    CHECK_EQ_U64(st.files, live);
    CHECK_EQ_U64(st.logical_bytes, logical);
    fprintf(stderr,
            "    deleted %zu; live %zu files, logical %" PRIu64 " B, physical %" PRIu64
            " B, %" PRIu64 " blocks, %" PRIu64 " checkpoints\n",
            deleted, live, st.logical_bytes, st.physical_bytes, st.unique_blocks, st.checkpoints);
    CHECK(st.checkpoints > 10); /* the 48-frame pool forced many */
    uint64_t physical = st.physical_bytes, blocks = st.unique_blocks;
    REQUIRE(nx_engine_close(e) == 0);

    /* Reopen with a different pool size: everything is still there. */
    nx_engine_config cfg;
    cfg_for(dir, 300, &cfg);
    REQUIRE(nx_engine_open(&cfg, &e) == 0);
    nx_engine_get_stats(e, &st);
    CHECK_EQ_U64(st.files, live);
    CHECK_EQ_U64(st.logical_bytes, logical);
    CHECK_EQ_U64(st.physical_bytes, physical);
    CHECK_EQ_U64(st.unique_blocks, blocks);
    for (int i = 0; i < N; i++)
        if (m[i].live)
            CHECK(read_matches(e, m[i].id, m[i].data, m[i].len));
    CHECK(fsck_ok(e, &r) == 0);

    /* New ids never collide with ids used before the restart. */
    nx_commit_result res;
    upload_info info;
    REQUIRE(upload(e, 1, "after-restart.csv", m[0].data, m[0].len, NX_CHUNKER_CDC, &res, &info) == 0);
    free(info.refs);
    for (int i = 0; i < N; i++)
        CHECK(res.file_id != m[i].id);

    /* Delete everything: every refcount goes back to zero (fsck checks I2). */
    for (int i = 0; i < N; i++)
        if (m[i].live)
            CHECK(nx_engine_delete_file(e, m[i].id) == 0);
    CHECK(nx_engine_delete_file(e, res.file_id) == 0);
    CHECK(fsck_ok(e, &r) == 0);
    nx_engine_get_stats(e, &st);
    CHECK_EQ_U64(st.files, 0);
    CHECK_EQ_U64(st.logical_bytes, 0);
    REQUIRE(nx_engine_close(e) == 0);
    for (int i = 0; i < N; i++)
        free(m[i].data);
}

/* ---- error paths ------------------------------------------------------------------ */

static void test_commit_errors(void)
{
    char dir[600];
    nx_engine *e = open_fresh("eng_err", dir, sizeof dir, 64);
    REQUIRE(e);
    nx_splitmix rng = {5};
    uint8_t *data = make_content(&rng, 50000);
    nx_chunk_ref refs[64];
    uint32_t n = 0;
    for (size_t pos = 0; pos < 50000; pos += refs[n++].len) {
        refs[n].len = (uint32_t)nx_cdc_cut(data + pos, 50000 - pos);
        nx_sha256(data + pos, refs[n].len, refs[n].hash);
    }
    nx_commit_meta m = {.owner_id = 1, .name = "a.csv", .chunker = NX_CHUNKER_CDC, .size = 50000};
    nx_sha256(data, 50000, m.sha256);
    nx_commit_result res;

    /* Nothing stored yet: every distinct block is reported missing. */
    CHECK(nx_engine_commit_file(e, &m, refs, n, &res) == NX_ENGINE_MISSING);
    CHECK_EQ_U64(res.missing_count, n);
    nx_commit_result_free(&res);

    /* A block whose bytes do not match its hash is refused. */
    nx_block_in b = {data, refs[0].len, {0}};
    memcpy(b.hash, refs[0].hash, 32);
    b.hash[5] ^= 1;
    nx_store_result sr;
    CHECK(nx_engine_store_blocks(e, &b, 1, &sr) == NX_ENGINE_BAD_HASH);
    b.hash[5] ^= 1;
    b.len = 0;
    CHECK(nx_engine_store_blocks(e, &b, 1, &sr) == NX_ENGINE_INVALID);
    b.len = refs[0].len;
    REQUIRE(nx_engine_store_blocks(e, &b, 1, &sr) == 0);
    CHECK(sr.stored == 1);
    REQUIRE(nx_engine_store_blocks(e, &b, 1, &sr) == 0); /* again: deduplicated */
    CHECK(sr.stored == 0 && sr.duplicate == 1);

    CHECK(nx_engine_commit_file(e, &m, refs, n, &res) == NX_ENGINE_MISSING);
    CHECK_EQ_U64(res.missing_count, n - 1);
    nx_commit_result_free(&res);

    /* Store the rest, then commit with a wrong whole-file hash. */
    size_t pos = refs[0].len;
    for (uint32_t i = 1; i < n; pos += refs[i].len, i++) {
        nx_block_in bi = {data + pos, refs[i].len, {0}};
        memcpy(bi.hash, refs[i].hash, 32);
        REQUIRE(nx_engine_store_blocks(e, &bi, 1, &sr) == 0);
    }
    m.sha256[0] ^= 1;
    CHECK(nx_engine_commit_file(e, &m, refs, n, &res) == NX_ENGINE_BAD_HASH);
    m.sha256[0] ^= 1;

    /* Malformed requests. */
    m.size = 49999; /* chunk lengths do not add up */
    CHECK(nx_engine_commit_file(e, &m, refs, n, &res) == NX_ENGINE_INVALID);
    m.size = 50000;
    char longname[300];
    memset(longname, 'n', 299);
    longname[299] = '\0';
    m.name = longname;
    CHECK(nx_engine_commit_file(e, &m, refs, n, &res) == NX_ENGINE_INVALID);
    m.name = "";
    CHECK(nx_engine_commit_file(e, &m, refs, n, &res) == NX_ENGINE_INVALID);
    m.name = "a.csv";
    refs[1].len += 1; /* a known hash with another length */
    m.size += 1;
    CHECK(nx_engine_commit_file(e, &m, refs, n, &res) == NX_ENGINE_INVALID);
    refs[1].len -= 1;
    m.size -= 1;

    REQUIRE(nx_engine_commit_file(e, &m, refs, n, &res) == 0);
    CHECK(res.version == 1);
    uint64_t first = res.file_id;
    REQUIRE(nx_engine_commit_file(e, &m, refs, n, &res) == 0); /* same name: version 2 */
    CHECK(res.version == 2);
    nx_file f;
    REQUIRE(nx_catalog_file_by_id(nx_engine_catalog(e), res.file_id, &f));
    CHECK(f.prev_version_id == first);
    CHECK(nx_engine_delete_file(e, 999999) == NX_ENGINE_NOT_FOUND);
    CHECK(nx_engine_set_file_flags(e, first, NX_FILE_SHARED) == 0);
    REQUIRE(nx_catalog_file_by_id(nx_engine_catalog(e), first, &f));
    CHECK(f.flags == NX_FILE_SHARED);

    nx_fsck_report r;
    CHECK(fsck_ok(e, &r) == 0);
    /* A refcount-0 block (stored, never committed) is fine for fsck. */
    uint8_t lone[3000];
    memset(lone, 7, sizeof lone);
    nx_block_in bl = {lone, sizeof lone, {0}};
    nx_sha256(lone, sizeof lone, bl.hash);
    REQUIRE(nx_engine_store_blocks(e, &bl, 1, &sr) == 0);
    CHECK(fsck_ok(e, &r) == 0);
    REQUIRE(nx_engine_close(e) == 0);

    /* An existing data dir is not re-initialized; a missing one cannot open. */
    CHECK(nx_engine_create(dir) != 0);
    nx_engine_config cfg;
    char other[700];
    snprintf(other, sizeof other, "%s-nothing-here", dir);
    cfg_for(other, 64, &cfg);
    CHECK(nx_engine_open(&cfg, &e) != 0);
    free(data);
}

/* ---- fsck detects damage ------------------------------------------------------------ */

static int flip_byte(const char *path, uint64_t off)
{
    plat_file *f;
    if (plat_file_open(path, PLAT_O_WRITE, &f) != 0)
        return -1;
    uint8_t b;
    int rc = plat_pread(f, &b, 1, off) == 1 ? 0 : -1;
    b ^= 0x20;
    if (rc == 0)
        rc = plat_pwrite_all(f, &b, 1, off);
    plat_file_close(f);
    return rc;
}

/* Adds 1 to the refcount of the first referenced entry in buckets.bin and
 * re-seals the page CRC, as a buggy apply would have left it. */
static int bump_a_refcount(const char *dir)
{
    char path[700];
    snprintf(path, sizeof path, "%s/index/buckets.bin", dir);
    plat_file *f;
    if (plat_file_open(path, PLAT_O_WRITE, &f) != 0)
        return -1;
    uint64_t size;
    plat_file_size(f, &size);
    uint8_t page[NX_PAGE_SIZE];
    int rc = -1;
    for (uint64_t p = 0; p < size / NX_PAGE_SIZE && rc != 0; p++) {
        if (plat_pread(f, page, NX_PAGE_SIZE, p * NX_PAGE_SIZE) != NX_PAGE_SIZE)
            break;
        unsigned count = (unsigned)(page[10] | (page[11] << 8));
        for (unsigned i = 0; i < count; i++) {
            uint8_t *ref = page + NX_BUCKET_HEADER + i * NX_INDEX_ENTRY_SIZE + 44;
            if (ref[0] | ref[1] | ref[2] | ref[3]) {
                ref[0]++;
                nx_page_seal_crc(page);
                rc = plat_pwrite_all(f, page, NX_PAGE_SIZE, p * NX_PAGE_SIZE);
                break;
            }
        }
    }
    plat_file_close(f);
    return rc;
}

static void test_fsck_detects_damage(void)
{
    char dir[600];
    nx_engine *e = open_fresh("eng_damage", dir, sizeof dir, 64);
    REQUIRE(e);
    nx_splitmix rng = {11};
    uint8_t *a = make_content(&rng, 40000), *b = make_content(&rng, 40000);
    nx_commit_result ra, rb;
    upload_info info;
    REQUIRE(upload(e, 1, "a.csv", a, 40000, NX_CHUNKER_CDC, &ra, &info) == 0);
    free(info.refs);
    REQUIRE(upload(e, 1, "b.csv", b, 40000, NX_CHUNKER_CDC, &rb, &info) == 0);
    free(info.refs);
    nx_fsck_report r;
    CHECK(fsck_ok(e, &r) == 0);
    REQUIRE(nx_engine_close(e) == 0);

    /* I1: flip a data byte of the first block in container 1 (file a). */
    char path[700];
    snprintf(path, sizeof path, "%s/containers/c00000001.dat", dir);
    REQUIRE(flip_byte(path, 64 + 40 + 100) == 0);
    nx_engine_config cfg;
    cfg_for(dir, 64, &cfg);
    REQUIRE(nx_engine_open(&cfg, &e) == 0);
    CHECK(nx_engine_fsck(e, &r) == 1);
    CHECK(r.i1_errors == 1 && r.i2_errors == 0 && r.i3_errors == 0);
    fprintf(stderr, "    detected: %s\n", r.first_error);
    CHECK(nx_engine_read_file(e, ra.file_id, NULL, NULL) == NX_ENGINE_CORRUPT);
    CHECK(read_matches(e, rb.file_id, b, 40000)); /* the other file is intact */
    REQUIRE(nx_engine_close(e) == 0);
    REQUIRE(flip_byte(path, 64 + 40 + 100) == 0); /* undo */

    /* I2: a refcount one too high. */
    REQUIRE(bump_a_refcount(dir) == 0);
    REQUIRE(nx_engine_open(&cfg, &e) == 0);
    CHECK(nx_engine_fsck(e, &r) == 1);
    CHECK(r.i1_errors == 0 && r.i2_errors == 1 && r.i3_errors == 0);
    fprintf(stderr, "    detected: %s\n", r.first_error);
    REQUIRE(nx_engine_close(e) == 0);

    /* I3: with verify_on_commit off, a commit with a wrong whole-file hash
     * gets in; fsck must notice. (Fresh dir, so I2 above does not interfere.) */
    e = open_fresh("eng_damage_i3", dir, sizeof dir, 64);
    REQUIRE(e);
    REQUIRE(nx_engine_close(e) == 0);
    cfg_for(dir, 64, &cfg);
    cfg.verify_on_commit = false;
    REQUIRE(nx_engine_open(&cfg, &e) == 0);
    REQUIRE(upload(e, 1, "a.csv", a, 40000, NX_CHUNKER_CDC, &ra, &info) == 0);
    nx_commit_meta m = {.owner_id = 1, .name = "liar.csv", .chunker = NX_CHUNKER_CDC, .size = 40000};
    nx_sha256(b, 40000, m.sha256); /* hash of other bytes */
    REQUIRE(nx_engine_commit_file(e, &m, info.refs, info.n, &rb) == 0);
    free(info.refs);
    CHECK(nx_engine_fsck(e, &r) == 1);
    CHECK(r.i1_errors == 0 && r.i2_errors == 0 && r.i3_errors == 1);
    fprintf(stderr, "    detected: %s\n", r.first_error);
    CHECK(nx_engine_read_file(e, rb.file_id, NULL, NULL) == NX_ENGINE_CORRUPT);
    REQUIRE(nx_engine_close(e) == 0);
    free(a);
    free(b);
}

/* ---- users ---------------------------------------------------------------------------- */

static void test_users(void)
{
    char dir[600];
    nx_engine *e = open_fresh("eng_users", dir, sizeof dir, 64);
    REQUIRE(e);
    nx_user u;
    memset(&u, 0, sizeof u);
    snprintf(u.username, sizeof u.username, "admin");
    snprintf(u.display, sizeof u.display, "Lab Admin");
    u.role = NX_ROLE_ADMIN;
    REQUIRE(nx_engine_add_user(e, &u) == 0);
    CHECK(u.id == 1);
    nx_user v = u;
    snprintf(v.username, sizeof v.username, "riya");
    v.role = NX_ROLE_STUDENT;
    REQUIRE(nx_engine_add_user(e, &v) == 0);
    CHECK(v.id == 2);
    nx_user dup = v;
    CHECK(nx_engine_add_user(e, &dup) == NX_ENGINE_EXISTS);
    v.status = NX_STATUS_DISABLED;
    REQUIRE(nx_engine_update_user(e, &v) == 0);
    nx_user rename = v;
    snprintf(rename.username, sizeof rename.username, "admin");
    CHECK(nx_engine_update_user(e, &rename) == NX_ENGINE_EXISTS);
    nx_user ghost = v;
    ghost.id = 77;
    CHECK(nx_engine_update_user(e, &ghost) == NX_ENGINE_NOT_FOUND);
    REQUIRE(nx_engine_close(e) == 0);

    nx_engine_config cfg;
    cfg_for(dir, 64, &cfg);
    REQUIRE(nx_engine_open(&cfg, &e) == 0);
    nx_user got;
    CHECK(nx_catalog_user_by_name(nx_engine_catalog(e), "riya", &got));
    CHECK(got.id == 2 && got.status == NX_STATUS_DISABLED);
    nx_user w;
    memset(&w, 0, sizeof w);
    snprintf(w.username, sizeof w.username, "kabir");
    REQUIRE(nx_engine_add_user(e, &w) == 0);
    CHECK(w.id == 3); /* ids continue after a restart */
    REQUIRE(nx_engine_close(e) == 0);
}

/* ---- concurrency -------------------------------------------------------------------------- */

typedef struct {
    nx_engine *e;
    const uint8_t *base;
    size_t base_len;
    int thread;
    int failures;
} worker;

static void *upload_worker(void *arg)
{
    worker *w = arg;
    nx_splitmix rng = {1000 + (uint64_t)w->thread};
    for (int i = 0; i < 12; i++) {
        size_t len = 0;
        /* Everyone edits the same base file, and every third upload is the
         * base itself: lots of blocks are stored by several threads at once.
         * Every fourth is new content of this thread's own, so the index
         * outgrows the 16-frame pool while other threads read and apply. */
        uint8_t *data;
        if (i % 3 == 0) {
            data = NULL;
        } else if (i % 4 == 1) {
            len = 150000;
            data = make_content(&rng, len);
        } else {
            data = edit_copy(&rng, w->base, w->base_len, &len);
        }
        const uint8_t *src = data ? data : w->base;
        if (!data)
            len = w->base_len;
        char name[64];
        snprintf(name, sizeof name, "t%d-%d.csv", w->thread, i % 4);
        nx_commit_result res;
        upload_info info;
        if (upload(w->e, (uint32_t)w->thread + 1, name, src, len, NX_CHUNKER_CDC, &res, &info) != 0 ||
            !read_matches(w->e, res.file_id, src, len))
            w->failures++;
        free(info.refs);
        if (i % 5 == 4 && nx_engine_delete_file(w->e, res.file_id) != 0)
            w->failures++;
        free(data);
    }
    return NULL;
}

static void test_concurrent_uploads(void)
{
    char dir[600];
    nx_engine *e = open_fresh("eng_conc", dir, sizeof dir, 16);
    REQUIRE(e);
    nx_splitmix rng = {3};
    size_t base_len = 200000;
    uint8_t *base = make_content(&rng, base_len);
    enum { T = 6 };
    pthread_t th[T];
    worker w[T];
    for (int i = 0; i < T; i++) {
        w[i] = (worker){e, base, base_len, i, 0};
        REQUIRE(pthread_create(&th[i], NULL, upload_worker, &w[i]) == 0);
    }
    for (int i = 0; i < T; i++) {
        pthread_join(th[i], NULL);
        CHECK(w[i].failures == 0);
    }
    nx_fsck_report r;
    CHECK(fsck_ok(e, &r) == 0);
    nx_engine_stats st;
    nx_engine_get_stats(e, &st);
    fprintf(stderr, "    %d threads: %" PRIu64 " files, %" PRIu64 " checkpoints, fsck clean\n", T,
            st.files, st.checkpoints);
    REQUIRE(nx_engine_close(e) == 0);
    free(base);
}

static const nx_test_case TESTS[] = {
    NX_CASE(test_round_trip_200_files),
    NX_CASE(test_commit_errors),
    NX_CASE(test_fsck_detects_damage),
    NX_CASE(test_users),
    NX_CASE(test_concurrent_uploads),
};

NX_TEST_MAIN(TESTS)
