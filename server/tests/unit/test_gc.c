/*
 * test_gc.c — cleanup, compaction, integrity scan, index rebuild (M12,
 * SPEC §10, DECISIONS D-048):
 *   - cleanup keeps referenced blocks, blocks inside the grace period
 *     (exactly at the boundary), and blocks an open upload declared
 *   - compaction rewrites containers below 50% live, every file stays
 *     byte-exact, disk use drops, repeated runs converge
 *   - a crash at every step of compaction (copy, commit, checkpoint,
 *     delete) recovers to a correct store, and the next cleanup finishes
 *     the job without leftovers
 *   - the store barrier: compaction waits for a store that appended into a
 *     container but has not logged CHUNK_ADD yet
 *   - uploads, deletes, and downloads keep working while compaction runs
 *   - the scan finds one flipped byte and names both files that use the
 *     damaged block
 *   - --rebuild-index restores a working index from containers and
 *     recipes, including log records written after the last checkpoint,
 *     and reports blocks that exist nowhere
 */
#include "engine/engine_internal.h"
#include "nx_engine_util.h"
#include "util/failpoint.h"

#include <pthread.h>

/* ---- helpers --------------------------------------------------------------- */

typedef struct {
    uint64_t id;
    uint8_t *data;
    size_t len;
} tfile;

static nx_engine *reopen(const char *dir, size_t pool, uint64_t container_size)
{
    nx_fail_reset();
    nx_engine_config cfg;
    cfg_for(dir, pool, &cfg);
    if (container_size)
        cfg.container_size = container_size;
    nx_engine *e = NULL;
    return nx_engine_open(&cfg, &e) == 0 ? e : NULL;
}

static nx_engine *fresh(const char *name, char *dir, size_t n, size_t pool, uint64_t container_size)
{
    nx_test_tmpdir(name, dir, n);
    if (nx_engine_create(dir) != 0)
        return NULL;
    return reopen(dir, pool, container_size);
}

/* Uploads like a client; a 409 (a block freed meanwhile) is retried. */
static int put(nx_engine *e, const char *name, const uint8_t *d, size_t len, uint64_t *id)
{
    for (int attempt = 0; attempt < 20; attempt++) {
        nx_commit_result res;
        upload_info info;
        int rc = upload(e, 1, name, d, len, NX_CHUNKER_CDC, &res, &info);
        free(info.refs);
        nx_commit_result_free(&res);
        if (rc == 0) {
            *id = res.file_id;
            return 0;
        }
        if (rc != NX_ENGINE_MISSING)
            return rc;
    }
    return -1;
}

static uint64_t container_bytes(nx_engine *e)
{
    nx_container_info *ci;
    size_t n;
    if (nx_engine_containers(e, &ci, &n) != 0)
        return 0;
    uint64_t b = 0;
    for (size_t i = 0; i < n; i++)
        b += ci[i].file_bytes;
    free(ci);
    return b;
}

static size_t container_count(nx_engine *e)
{
    nx_container_info *ci;
    size_t n;
    if (nx_engine_containers(e, &ci, &n) != 0)
        return 0;
    free(ci);
    return n;
}

/* Below the active container: how many have nothing live (leftovers of a
 * dropped container) and how many are under 50% live (compaction work). */
static void layout_check(nx_engine *e, size_t *empty, size_t *under_half)
{
    *empty = *under_half = 0;
    nx_block_span *sp;
    size_t ns;
    nx_container_info *ci;
    size_t nc;
    if (nx_engine_block_spans(e, &sp, &ns) != 0 || nx_engine_containers(e, &ci, &nc) != 0) {
        *empty = *under_half = 9999;
        return;
    }
    uint32_t active = nx_containers_active_id(e->ct);
    for (size_t c = 0; c < nc; c++) {
        if (ci[c].id >= active)
            continue;
        uint64_t live = 0, cnt = 0;
        for (size_t s = 0; s < ns; s++)
            if (sp[s].container_id == ci[c].id) {
                live += NX_RECORD_HEADER + sp[s].len;
                cnt++;
            }
        uint64_t data = ci[c].file_bytes - NX_CONTAINER_HEADER;
        if (cnt == 0)
            (*empty)++;
        else if (live * 2 < data)
            (*under_half)++;
    }
    free(sp);
    free(ci);
}

static bool all_match(nx_engine *e, const tfile *f, size_t n)
{
    bool ok = true;
    for (size_t i = 0; i < n; i++)
        if (f[i].id)
            ok = read_matches(e, f[i].id, f[i].data, f[i].len) && ok;
    return ok;
}

static int run_gc(nx_engine *e, uint32_t grace, nx_gc_report *r)
{
    nx_gc_options o = {.grace_s = grace};
    return nx_engine_gc(e, &o, r);
}

/* Runs cleanup until a run has nothing left to do (at most 6 runs). */
static bool converge(nx_engine *e, uint64_t *reclaimed)
{
    for (int i = 0; i < 6; i++) {
        nx_gc_report r;
        if (run_gc(e, 0, &r) != 0)
            return false;
        if (reclaimed)
            *reclaimed += r.bytes_reclaimed;
        if (r.containers_compacted == 0 && r.blocks_moved == 0 && r.blocks_freed == 0)
            return true;
    }
    return false;
}

/* 16 files of 100 KB random data in 256 KB containers; delete 12 of them,
 * leaving every container about a quarter live. */
#define NFILES 16
static int build_sparse_store(nx_engine *e, tfile *f)
{
    for (int i = 0; i < NFILES; i++) {
        f[i].len = 100000;
        f[i].data = nx_splitmix_bytes(1000 + (uint64_t)i, f[i].len);
        char name[32];
        snprintf(name, sizeof name, "f%02d.bin", i);
        if (put(e, name, f[i].data, f[i].len, &f[i].id) != 0)
            return -1;
    }
    for (int i = 0; i < NFILES; i++)
        if (i % 4 != 3) {
            if (nx_engine_delete_file(e, f[i].id) != 0)
                return -1;
            f[i].id = 0;
        }
    return 0;
}

static void free_files(tfile *f, size_t n)
{
    for (size_t i = 0; i < n; i++)
        free(f[i].data);
}

/* ---- cleanup rules ------------------------------------------------------------- */

typedef struct {
    uint8_t hash[32];
    int asked;
} keep_one;

static bool keep_cb(const uint8_t hash[32], void *ctx)
{
    keep_one *k = ctx;
    k->asked++;
    return memcmp(hash, k->hash, 32) == 0;
}

static void test_cleanup_rules(void)
{
    char dir[600];
    nx_engine *e = fresh("gc_rules", dir, sizeof dir, 256, 0);
    REQUIRE(e != NULL);
    nx_splitmix rng = {5};
    size_t len = 150000;
    uint8_t *a = make_content(&rng, len);
    uint8_t *b = malloc(len);
    REQUIRE(b != NULL);
    memcpy(b, a, len);
    memcpy(b + 70000, "an edit in the middle of the second copy", 40);
    uint64_t ida, idb;
    REQUIRE(put(e, "a.csv", a, len, &ida) == 0);
    REQUIRE(put(e, "b.csv", b, len, &idb) == 0);

    /* Three blocks stored by an upload that never commits. */
    uint8_t *orphan = nx_splitmix_bytes(77, 3 * 4096);
    nx_block_in in[3];
    for (int i = 0; i < 3; i++) {
        in[i].data = orphan + i * 4096;
        in[i].len = 4096;
        nx_sha256(in[i].data, 4096, in[i].hash);
    }
    nx_store_result sr;
    REQUIRE(nx_engine_store_blocks(e, in, 3, &sr) == 0 && sr.stored == 3);
    nx_index_entry ent;
    REQUIRE(nx_engine_lookup(e, in[0].hash, &ent) == 0);
    uint64_t born = ent.created_at;

    nx_gc_report r;
    /* One second before the grace period ends nothing is freed... */
    nx_gc_options o = {.grace_s = 3600, .now_s = born + 3599};
    REQUIRE(nx_engine_gc(e, &o, &r) == 0);
    CHECK_EQ_U64(r.blocks_freed, 0);
    /* ...and exactly at its end (age == grace) the orphans go, except the
     * one an open upload still declares. */
    keep_one k;
    memcpy(k.hash, in[1].hash, 32);
    k.asked = 0;
    o = (nx_gc_options){.grace_s = 3600, .now_s = born + 3600, .keep = keep_cb, .keep_ctx = &k};
    REQUIRE(nx_engine_gc(e, &o, &r) == 0);
    CHECK_EQ_U64(r.blocks_freed, 2);
    CHECK(k.asked == 3);
    CHECK(nx_engine_lookup(e, in[0].hash, &ent) == NX_ENGINE_NOT_FOUND);
    CHECK(nx_engine_lookup(e, in[1].hash, &ent) == 0);
    CHECK(nx_engine_lookup(e, in[2].hash, &ent) == NX_ENGINE_NOT_FOUND);
    nx_engine_stats before;
    nx_engine_get_stats(e, &before);

    /* Delete a: its own blocks drop to refcount 0, the blocks b shares
     * with it stay referenced. */
    nx_chunk_ref *ra;
    nx_file fa;
    REQUIRE(nx_engine_file_recipe(e, ida, &fa, &ra) == 0);
    REQUIRE(nx_engine_delete_file(e, ida) == 0);
    size_t unique_a = 0;
    for (uint32_t i = 0; i < fa.chunk_count; i++) {
        REQUIRE(nx_engine_lookup(e, ra[i].hash, &ent) == 0);
        if (ent.refcount == 0)
            unique_a++;
    }
    CHECK(unique_a >= 1 && unique_a <= 4); /* one 40-byte edit touches 1-2 CDC blocks */

    /* An hour later a's own blocks go too; still not the declared one. */
    k.asked = 0;
    o = (nx_gc_options){.grace_s = 3600, .now_s = plat_now_unix_s() + 3600, .keep = keep_cb,
                        .keep_ctx = &k};
    REQUIRE(nx_engine_gc(e, &o, &r) == 0);
    CHECK_EQ_U64(r.blocks_freed, unique_a);
    CHECK(k.asked == (int)(unique_a + 1));
    CHECK(nx_engine_lookup(e, in[1].hash, &ent) == 0);
    for (uint32_t i = 0; i < fa.chunk_count; i++) {
        int lr = nx_engine_lookup(e, ra[i].hash, &ent);
        CHECK(lr == NX_ENGINE_NOT_FOUND || ent.refcount > 0);
    }
    nx_engine_stats after;
    nx_engine_get_stats(e, &after);
    CHECK_EQ_U64(before.unique_blocks - after.unique_blocks, r.blocks_freed);
    CHECK(read_matches(e, idb, b, len));
    nx_fsck_report fr;
    CHECK(fsck_ok(e, &fr) == 0);

    /* The kept block goes once nobody declares it; freed blocks stay
     * freed after a restart. */
    o = (nx_gc_options){.grace_s = 0};
    REQUIRE(nx_engine_gc(e, &o, &r) == 0);
    CHECK_EQ_U64(r.blocks_freed, 1);
    REQUIRE(nx_engine_close(e) == 0);
    e = reopen(dir, 256, 0);
    REQUIRE(e != NULL);
    CHECK(nx_engine_lookup(e, in[1].hash, &ent) == NX_ENGINE_NOT_FOUND);
    CHECK(read_matches(e, idb, b, len));
    CHECK(fsck_ok(e, &fr) == 0);
    fprintf(stderr, "    deleted file had %zu own blocks; freed them and 3 orphans; shared blocks kept\n",
            unique_a);
    REQUIRE(nx_engine_close(e) == 0);
    free(ra);
    free(a);
    free(b);
    free(orphan);
}

/* ---- compaction ------------------------------------------------------------------ */

static void test_compaction(void)
{
    char dir[600];
    nx_engine *e = fresh("gc_compact", dir, sizeof dir, 256, 0);
    REQUIRE(e != NULL);
    tfile f[NFILES] = {{0}};
    REQUIRE(build_sparse_store(e, f) == 0);
    uint64_t bytes0 = container_bytes(e);
    size_t count0 = container_count(e);
    nx_engine_stats s0;
    nx_engine_get_stats(e, &s0);

    nx_gc_report r;
    REQUIRE(run_gc(e, 0, &r) == 0);
    uint64_t bytes1 = container_bytes(e);
    fprintf(stderr,
            "    %zu containers, %" PRIu64 " KB -> freed %" PRIu64 " blocks, compacted %" PRIu64
            ", moved %" PRIu64 " blocks (%" PRIu64 " KB), reclaimed %" PRIu64 " KB -> %zu containers, %" PRIu64
            " KB\n",
            count0, bytes0 / 1024, r.blocks_freed, r.containers_compacted, r.blocks_moved,
            r.bytes_moved / 1024, r.bytes_reclaimed / 1024, container_count(e), bytes1 / 1024);
    CHECK(r.blocks_freed > 0);
    CHECK(r.containers_compacted >= 4);
    CHECK(r.blocks_moved > 0);
    CHECK_EQ_U64(r.containers_skipped, 0);
    CHECK(bytes1 < bytes0 / 2);
    /* Reclaimed counts deleted files minus copies; a fresh container's
     * 64-byte header (rotation, or a seal while copying) is the rest. */
    CHECK(r.bytes_reclaimed >= bytes0 - bytes1 &&
          r.bytes_reclaimed - (bytes0 - bytes1) <= 4 * NX_CONTAINER_HEADER);
    CHECK(all_match(e, f, NFILES));
    nx_fsck_report fr;
    CHECK(fsck_ok(e, &fr) == 0);
    nx_engine_stats s1;
    nx_engine_get_stats(e, &s1);
    CHECK_EQ_U64(s1.logical_bytes, 4 * 100000);
    CHECK_EQ_U64(s0.unique_blocks - s1.unique_blocks, r.blocks_freed);

    /* Converges, and a restart keeps every move. */
    CHECK(converge(e, NULL));
    size_t empty, under;
    layout_check(e, &empty, &under);
    CHECK(empty == 0 && under == 0);
    REQUIRE(nx_engine_close(e) == 0);
    e = reopen(dir, 256, 0);
    REQUIRE(e != NULL);
    CHECK(all_match(e, f, NFILES));
    CHECK(fsck_ok(e, &fr) == 0);
    REQUIRE(nx_engine_close(e) == 0);
    free_files(f, NFILES);
}

/* Compaction leaves a container that holds a damaged live block alone. */
static void test_compaction_skips_damaged(void)
{
    char dir[600];
    nx_engine *e = fresh("gc_damaged", dir, sizeof dir, 256, 0);
    REQUIRE(e != NULL);
    tfile f[NFILES] = {{0}};
    REQUIRE(build_sparse_store(e, f) == 0);
    /* Damage one byte of a block of file 3 (which is kept). */
    nx_chunk_ref *rc3;
    REQUIRE(nx_engine_file_recipe(e, f[3].id, NULL, &rc3) == 0);
    nx_index_entry ent;
    REQUIRE(nx_engine_lookup(e, rc3[2].hash, &ent) == 0);
    char path[800];
    snprintf(path, sizeof path, "%s/containers/c%08" PRIu32 ".dat", dir, ent.container_id);
    plat_file *pf;
    REQUIRE(plat_file_open(path, PLAT_O_WRITE, &pf) == 0);
    uint8_t byte;
    uint64_t at = (uint64_t)ent.offset + NX_RECORD_HEADER + 100;
    REQUIRE(plat_pread(pf, &byte, 1, at) == 1);
    byte ^= 0x40;
    REQUIRE(plat_pwrite_all(pf, &byte, 1, at) == 0);
    plat_file_close(pf);

    nx_gc_report r;
    REQUIRE(run_gc(e, 0, &r) == 0);
    CHECK_EQ_U64(r.containers_skipped, 1);
    CHECK(r.containers_compacted >= 3);
    plat_file *still;
    CHECK(plat_file_open(path, PLAT_O_READ, &still) == 0); /* the evidence is kept */
    if (still)
        plat_file_close(still);
    for (int i = 0; i < NFILES; i++)
        if (f[i].id && i != 3)
            CHECK(read_matches(e, f[i].id, f[i].data, f[i].len));
    CHECK(nx_engine_read_file(e, f[3].id, NULL, NULL) == NX_ENGINE_CORRUPT);
    REQUIRE(nx_engine_close(e) == 0);
    free(rc3);
    free_files(f, NFILES);
}

/* ---- crash at every step of compaction ------------------------------------------------- */

static void test_compaction_crash_points(void)
{
    static const struct {
        const char *point;
        uint32_t skip;
    } CASES[] = {
        {"wal.append.torn", 0},              /* the first BLOCK_FREE transaction */
        {"txn.mid_apply", 0},                /* applying the frees */
        {"container.append.torn", 5},        /* copying a live block */
        {"gc.compact.after_copy", 0},        /* copies fsynced, moves not logged */
        {"gc.compact.after_commit", 0},      /* moves logged for one container */
        {"gc.compact.after_commit", 2},      /* ...for three containers */
        {"txn.mid_apply", 3},                /* applying the moves */
        {"ckpt.after_doublewrite", 0},       /* the checkpoint before deleting */
        {"gc.compact.after_checkpoint", 0},  /* checkpoint done, nothing deleted */
        {"gc.compact.before_delete", 2},     /* two old files deleted, the rest not */
    };
    size_t n = sizeof CASES / sizeof CASES[0];
    size_t ok = 0;
    for (size_t c = 0; c < n; c++) {
        char name[64], dir[600];
        snprintf(name, sizeof name, "gc_crash_%02zu", c);
        nx_engine *e = fresh(name, dir, sizeof dir, 256, 0);
        REQUIRE(e != NULL);
        tfile f[NFILES] = {{0}};
        REQUIRE(build_sparse_store(e, f) == 0);
        uint64_t bytes0 = container_bytes(e);

        nx_fail_arm(CASES[c].point, CASES[c].skip);
        nx_gc_report r;
        int grc = run_gc(e, 0, &r);
        bool crashed = nx_fail_crashed();
        nx_engine_abandon(e); /* kill -9 */
        e = reopen(dir, 256, 0);
        bool reopened = e != NULL;
        nx_fsck_report fr;
        bool good = crashed && grc != 0 && reopened && fsck_ok(e, &fr) == 0 && all_match(e, f, NFILES);
        /* The next cleanup finishes the job and leaves nothing behind. */
        uint64_t reclaimed = 0;
        size_t empty = 99, under = 99;
        if (good) {
            good = converge(e, &reclaimed);
            layout_check(e, &empty, &under);
            good = good && empty == 0 && under == 0 && fsck_ok(e, &fr) == 0 && all_match(e, f, NFILES);
        }
        uint64_t bytes1 = reopened ? container_bytes(e) : 0;
        good = good && bytes1 < bytes0 / 2;
        /* And once more after a clean restart. */
        if (good) {
            good = nx_engine_close(e) == 0;
            e = reopen(dir, 256, 0);
            good = good && e && fsck_ok(e, &fr) == 0 && all_match(e, f, NFILES);
        }
        fprintf(stderr, "    %-28s skip=%u: crashed=%d reopened=%d, then %" PRIu64 " -> %" PRIu64 " KB: %s\n",
                CASES[c].point, CASES[c].skip, crashed, reopened, bytes0 / 1024, bytes1 / 1024,
                good ? "ok" : "WRONG");
        ok += good;
        if (e)
            nx_engine_close(e);
        free_files(f, NFILES);
    }
    CHECK(ok == n);
}

/* ---- the store barrier ----------------------------------------------------------------- */

typedef struct {
    nx_engine *e;
    nx_block_in *in;
    size_t n;
    int rc;
    _Atomic int done;
} store_job;

static void *store_thread(void *arg)
{
    store_job *j = arg;
    nx_store_result sr;
    j->rc = nx_engine_store_blocks(j->e, j->in, j->n, &sr);
    atomic_store(&j->done, 1);
    return NULL;
}

typedef struct {
    nx_engine *e;
    nx_gc_report r;
    int rc;
    _Atomic int done;
} gc_job;

static void *gc_thread(void *arg)
{
    gc_job *j = arg;
    j->rc = run_gc(j->e, 0, &j->r);
    atomic_store(&j->done, 1);
    return NULL;
}

static void test_store_barrier(void)
{
    char dir[600];
    /* 64 KB containers: one store of 40 blocks seals two of them. */
    nx_engine *e = fresh("gc_barrier", dir, sizeof dir, 256, 64 * 1024);
    REQUIRE(e != NULL);
    size_t nb = 40;
    uint8_t *data = nx_splitmix_bytes(4242, nb * 4096);
    nx_block_in *in = malloc(nb * sizeof *in);
    nx_chunk_ref *refs = malloc(nb * sizeof *refs);
    REQUIRE(in && refs);
    for (size_t i = 0; i < nb; i++) {
        in[i].data = data + i * 4096;
        in[i].len = 4096;
        nx_sha256(in[i].data, 4096, in[i].hash);
        memcpy(refs[i].hash, in[i].hash, 32);
        refs[i].len = 4096;
    }
    /* Hold the transaction mutex: the store appends its blocks (sealing
     * containers on the way) and then waits to log CHUNK_ADD. */
    pthread_mutex_lock(&e->txn_mu);
    store_job sj = {e, in, nb, -1, 0};
    pthread_t st, gt;
    REQUIRE(pthread_create(&st, NULL, store_thread, &sj) == 0);
    for (int i = 0; i < 200 && nx_containers_active_id(e->ct) < 3; i++)
        plat_sleep_ms(10);
    CHECK(nx_containers_active_id(e->ct) >= 3);
    /* Cleanup now must wait for that store: containers 1 and 2 hold its
     * blocks, which the index does not know about yet. */
    gc_job gj = {e, {0}, -1, 0};
    REQUIRE(pthread_create(&gt, NULL, gc_thread, &gj) == 0);
    plat_sleep_ms(300);
    CHECK(atomic_load(&gj.done) == 0);
    CHECK(atomic_load(&sj.done) == 0);
    pthread_mutex_unlock(&e->txn_mu);
    pthread_join(st, NULL);
    pthread_join(gt, NULL);
    CHECK(sj.rc == 0 && gj.rc == 0);
    /* The blocks are still there, and a file can use all of them. */
    nx_commit_meta m = {.owner_id = 1, .name = "late.bin", .chunker = NX_CHUNKER_FIXED,
                        .size = nb * 4096};
    nx_sha256(data, nb * 4096, m.sha256);
    nx_commit_result res;
    CHECK(nx_engine_commit_file(e, &m, refs, (uint32_t)nb, &res) == 0);
    CHECK(read_matches(e, res.file_id, data, nb * 4096));
    nx_commit_result_free(&res);
    nx_fsck_report fr;
    CHECK(fsck_ok(e, &fr) == 0);
    REQUIRE(nx_engine_close(e) == 0);
    free(in);
    free(refs);
    free(data);
}

/* ---- uploads, deletes, downloads during compaction ------------------------------------ */

typedef struct {
    nx_engine *e;
    const tfile *stable;
    size_t nstable;
    int seed;
    _Atomic int *stop;
    int errors;
    int ops;
} worker;

static void *uploader(void *arg)
{
    worker *w = arg;
    nx_splitmix rng = {(uint64_t)w->seed};
    uint64_t mine[8] = {0};
    uint8_t *mine_data[8] = {0};
    size_t mine_len[8] = {0};
    while (!atomic_load(w->stop)) {
        int slot = (int)(nx_splitmix_next(&rng) % 8);
        if (mine[slot]) {
            if (!read_matches(w->e, mine[slot], mine_data[slot], mine_len[slot]) ||
                nx_engine_delete_file(w->e, mine[slot]) != 0)
                w->errors++;
            mine[slot] = 0;
            free(mine_data[slot]);
            mine_data[slot] = NULL;
        } else {
            mine_len[slot] = 20000 + (size_t)(nx_splitmix_next(&rng) % 60000);
            mine_data[slot] = nx_splitmix_bytes(nx_splitmix_next(&rng), mine_len[slot]);
            char name[32];
            snprintf(name, sizeof name, "w%d-%d", w->seed, slot);
            if (put(w->e, name, mine_data[slot], mine_len[slot], &mine[slot]) != 0)
                w->errors++;
        }
        w->ops++;
    }
    for (int i = 0; i < 8; i++) {
        if (mine[i] && !read_matches(w->e, mine[i], mine_data[i], mine_len[i]))
            w->errors++;
        free(mine_data[i]);
    }
    return NULL;
}

static void *reader(void *arg)
{
    worker *w = arg;
    size_t i = (size_t)w->seed;
    while (!atomic_load(w->stop)) {
        const tfile *f = &w->stable[i++ % w->nstable];
        if (f->id && !read_matches(w->e, f->id, f->data, f->len))
            w->errors++;
        w->ops++;
    }
    return NULL;
}

static void test_compaction_under_load(void)
{
    char dir[600];
    nx_engine *e = fresh("gc_load", dir, sizeof dir, 128, 128 * 1024);
    REQUIRE(e != NULL);
    tfile f[NFILES] = {{0}};
    REQUIRE(build_sparse_store(e, f) == 0);
    _Atomic int stop = 0;
    worker w[4] = {
        {e, f, NFILES, 11, &stop, 0, 0},
        {e, f, NFILES, 12, &stop, 0, 0},
        {e, f, NFILES, 3, &stop, 0, 0},
        {e, f, NFILES, 7, &stop, 0, 0},
    };
    pthread_t t[4];
    for (int i = 0; i < 4; i++)
        REQUIRE(pthread_create(&t[i], NULL, i < 2 ? uploader : reader, &w[i]) == 0);
    uint64_t compacted = 0, moved = 0;
    int gc_errors = 0;
    for (int run = 0; run < 8; run++) {
        nx_gc_report r;
        if (run_gc(e, 0, &r) != 0)
            gc_errors++;
        compacted += r.containers_compacted;
        moved += r.blocks_moved;
        plat_sleep_ms(30);
    }
    atomic_store(&stop, 1);
    for (int i = 0; i < 4; i++)
        pthread_join(t[i], NULL);
    fprintf(stderr,
            "    8 cleanups during %d uploads/deletes and %d downloads: compacted %" PRIu64
            " containers, moved %" PRIu64 " blocks\n",
            w[0].ops + w[1].ops, w[2].ops + w[3].ops, compacted, moved);
    CHECK(gc_errors == 0);
    CHECK(w[0].errors + w[1].errors + w[2].errors + w[3].errors == 0);
    CHECK(compacted > 0);
    CHECK(all_match(e, f, NFILES));
    nx_fsck_report fr;
    CHECK(fsck_ok(e, &fr) == 0);
    REQUIRE(nx_engine_close(e) == 0);
    e = reopen(dir, 128, 128 * 1024);
    REQUIRE(e != NULL);
    CHECK(fsck_ok(e, &fr) == 0);
    REQUIRE(nx_engine_close(e) == 0);
    free_files(f, NFILES);
}

/* ---- integrity scan ---------------------------------------------------------------------- */

typedef struct {
    uint64_t calls, last_done, total;
    bool monotonic;
} progress_log;

static void on_progress(const char *phase, uint64_t done, uint64_t total, void *ctx)
{
    progress_log *p = ctx;
    if (strcmp(phase, "scanning") != 0 || done < p->last_done)
        p->monotonic = false;
    p->calls++;
    p->last_done = done;
    p->total = total;
}

typedef struct {
    uint64_t ids[8];
    size_t n;
} id_list;

static int note_file(size_t which, const nx_file *f, void *ctx)
{
    id_list *l = ctx;
    if (which == 0 && l->n < 8)
        l->ids[l->n++] = f->id;
    return 0;
}

static void test_scan(void)
{
    char dir[600];
    nx_engine *e = fresh("gc_scan", dir, sizeof dir, 256, 0);
    REQUIRE(e != NULL);
    nx_splitmix rng = {9};
    size_t len = 300000;
    uint8_t *a = make_content(&rng, len), *c = make_content(&rng, 50000);
    uint8_t *b = malloc(len);
    REQUIRE(b != NULL);
    memcpy(b, a, len);
    memcpy(b + 250000, "edited near the end", 19);
    uint64_t ida, idb, idc;
    REQUIRE(put(e, "a.csv", a, len, &ida) == 0);
    REQUIRE(put(e, "b.csv", b, len, &idb) == 0);
    REQUIRE(put(e, "c.csv", c, 50000, &idc) == 0);
    nx_engine_stats st;
    nx_engine_get_stats(e, &st);

    nx_scan_report r;
    progress_log p = {0, 0, 0, true};
    REQUIRE(nx_engine_scan(e, on_progress, &p, &r) == 0);
    CHECK_EQ_U64(r.checked, st.unique_blocks);
    CHECK_EQ_U64(r.verified, st.unique_blocks);
    CHECK_EQ_U64(r.failures, 0);
    CHECK(p.monotonic && p.last_done == st.unique_blocks && p.total == st.unique_blocks);
    CHECK(p.calls >= 2 && p.calls <= 102); /* start + at most one per 1% */

    /* Flip one byte inside the first block of a (b uses it too). */
    nx_chunk_ref *ra;
    REQUIRE(nx_engine_file_recipe(e, ida, NULL, &ra) == 0);
    nx_index_entry ent;
    REQUIRE(nx_engine_lookup(e, ra[0].hash, &ent) == 0);
    CHECK(ent.refcount == 2);
    char path[800];
    snprintf(path, sizeof path, "%s/containers/c%08" PRIu32 ".dat", dir, ent.container_id);
    plat_file *pf;
    REQUIRE(plat_file_open(path, PLAT_O_WRITE, &pf) == 0);
    uint8_t byte;
    uint64_t at = (uint64_t)ent.offset + NX_RECORD_HEADER + 17;
    REQUIRE(plat_pread(pf, &byte, 1, at) == 1);
    byte ^= 0x01;
    REQUIRE(plat_pwrite_all(pf, &byte, 1, at) == 0);
    plat_file_close(pf);

    REQUIRE(nx_engine_scan(e, NULL, NULL, &r) == 0);
    CHECK_EQ_U64(r.failures, 1);
    CHECK_EQ_U64(r.verified, st.unique_blocks - 1);
    CHECK(r.n_items == 1 && memcmp(r.items[0].hash, ra[0].hash, 32) == 0);
    CHECK(r.items[0].reason == NX_SCAN_CRC && r.items[0].container_id == ent.container_id &&
          r.items[0].offset == ent.offset);
    id_list l = {{0}, 0};
    REQUIRE(nx_engine_files_using(e, (const uint8_t(*)[32]) & r.items[0].hash, 1, note_file, &l) == 0);
    CHECK(l.n == 2 && ((l.ids[0] == ida && l.ids[1] == idb) || (l.ids[0] == idb && l.ids[1] == ida)));
    /* Verify-on-read and fsck agree. */
    CHECK(nx_engine_read_file(e, ida, NULL, NULL) == NX_ENGINE_CORRUPT);
    CHECK(read_matches(e, idc, c, 50000));
    nx_fsck_report fr;
    CHECK(nx_engine_fsck(e, &fr) == 1 && fr.i1_errors == 2);

    /* Restoring the byte makes the scan clean again. */
    byte ^= 0x01;
    REQUIRE(plat_file_open(path, PLAT_O_WRITE, &pf) == 0);
    REQUIRE(plat_pwrite_all(pf, &byte, 1, at) == 0);
    plat_file_close(pf);
    REQUIRE(nx_engine_scan(e, NULL, NULL, &r) == 0);
    CHECK_EQ_U64(r.failures, 0);
    fprintf(stderr, "    %" PRIu64 " blocks verified; one flipped byte found, used by both copies\n",
            r.verified);
    REQUIRE(nx_engine_close(e) == 0);
    free(ra);
    free(a);
    free(b);
    free(c);
}

/* ---- --rebuild-index ------------------------------------------------------------------------ */

static void test_rebuild_index(void)
{
    char dir[600];
    nx_engine *e = fresh("gc_rebuild", dir, sizeof dir, 256, 0);
    REQUIRE(e != NULL);
    tfile f[NFILES] = {{0}};
    REQUIRE(build_sparse_store(e, f) == 0);
    REQUIRE(nx_engine_delete_file(e, f[3].id) == 0);
    f[3].id = 0;
    nx_gc_report gr;
    REQUIRE(run_gc(e, 0, &gr) == 0); /* moved blocks: old copies may remain on disk */
    /* Work after the last checkpoint, then a kill: the rebuild must replay it. */
    REQUIRE(nx_engine_checkpoint(e) == 0);
    f[0].len = 70000;
    f[0].data = realloc(f[0].data, f[0].len);
    nx_splitmix rng = {3};
    uint8_t *fresh_data = make_content(&rng, f[0].len);
    memcpy(f[0].data, fresh_data, f[0].len);
    free(fresh_data);
    REQUIRE(put(e, "late.csv", f[0].data, f[0].len, &f[0].id) == 0);
    nx_engine_stats before;
    nx_engine_get_stats(e, &before);
    nx_engine_abandon(e);

    nx_fail_reset();
    nx_engine_config cfg;
    cfg_for(dir, 256, &cfg);
    nx_rebuild_report rr;
    REQUIRE(nx_engine_rebuild_index(&cfg, &rr) == 0);
    fprintf(stderr,
            "    rebuilt from %" PRIu64 " containers: %" PRIu64 " records, %" PRIu64 " entries, %" PRIu64
            " duplicates, %" PRIu64 " files counted\n",
            rr.containers, rr.records, rr.entries, rr.duplicates, rr.files);
    CHECK_EQ_U64(rr.missing_blocks, 0);
    CHECK_EQ_U64(rr.damaged, 0);
    CHECK_EQ_U64(rr.files, 4); /* f07, f11, f15, late.csv */
    CHECK(rr.entries >= before.unique_blocks); /* freed blocks come back with refcount 0 */
    char old[800];
    snprintf(old, sizeof old, "%s/index/buckets.bin.old", dir);
    plat_file *of;
    CHECK(plat_file_open(old, PLAT_O_READ, &of) == 0);
    if (of)
        plat_file_close(of);

    e = reopen(dir, 256, 0);
    REQUIRE(e != NULL);
    nx_fsck_report fr;
    CHECK(fsck_ok(e, &fr) == 0);
    CHECK(all_match(e, f, NFILES));
    /* The refcount-0 blocks it brought back are ordinary cleanup work. */
    CHECK(converge(e, NULL));
    CHECK(fsck_ok(e, &fr) == 0);
    CHECK(all_match(e, f, NFILES));
    nx_engine_stats after;
    nx_engine_get_stats(e, &after);
    CHECK_EQ_U64(after.unique_blocks, before.unique_blocks);
    CHECK_EQ_U64(after.logical_bytes, before.logical_bytes);

    /* Lose a container that live files use: the rebuild says so. */
    nx_chunk_ref *r7;
    REQUIRE(nx_engine_file_recipe(e, f[7].id, NULL, &r7) == 0);
    nx_index_entry ent;
    REQUIRE(nx_engine_lookup(e, r7[0].hash, &ent) == 0);
    REQUIRE(nx_engine_close(e) == 0);
    char path[800];
    snprintf(path, sizeof path, "%s/containers/c%08" PRIu32 ".dat", dir, ent.container_id);
    REQUIRE(plat_remove(path) == 0);
    REQUIRE(nx_engine_rebuild_index(&cfg, &rr) == 1);
    CHECK(rr.missing_blocks > 0 && rr.files_affected >= 1);
    e = reopen(dir, 256, 0);
    REQUIRE(e != NULL);
    CHECK(nx_engine_fsck(e, &fr) == 1 && fr.i1_errors > 0);
    REQUIRE(nx_engine_close(e) == 0);
    free(r7);
    free_files(f, NFILES);
}

static const nx_test_case TESTS[] = {
    NX_CASE(test_cleanup_rules),
    NX_CASE(test_compaction),
    NX_CASE(test_compaction_skips_damaged),
    NX_CASE(test_compaction_crash_points),
    NX_CASE(test_store_barrier),
    NX_CASE(test_compaction_under_load),
    NX_CASE(test_scan),
    NX_CASE(test_rebuild_index),
};

NX_TEST_MAIN(TESTS)
