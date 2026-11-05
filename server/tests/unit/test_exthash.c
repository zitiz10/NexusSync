/*
 * test_exthash.c — extendible hashing block index (SPEC §7.2):
 *   - 1M random inserts with bucket splits and directory doubling, all
 *     found again, also after reopening from dir.bin + bucket pages
 *   - many hashes sharing their top bits (deep local depths)
 *   - random operations checked against a simple in-memory model
 *   - redo skips changes a page already contains
 *   - concurrent lookups and inserts under the directory lock
 */
#include "engine/exthash.h"
#include "nx_test.h"
#include "nx_test_util.h"

#include <pthread.h>

typedef struct {
    char dir[600];
    plat_file *f;
    nx_bufpool *bp;
    nx_exthash *ex;
    uint64_t checkpoints;
} env;

static int env_open(env *e, const char *name, size_t frames, const uint8_t *dir_img, size_t dir_len,
                    bool fresh_dir)
{
    if (fresh_dir)
        nx_test_tmpdir(name, e->dir, sizeof e->dir);
    char path[700];
    snprintf(path, sizeof path, "%s/buckets.bin", e->dir);
    if (plat_file_open(path, PLAT_O_WRITE | PLAT_O_CREATE, &e->f) != 0)
        return -1;
    if (nx_bufpool_create(frames, &e->bp) != 0 || nx_bufpool_attach(e->bp, 0, e->f) != 0)
        return -1;
    e->checkpoints = 0;
    return nx_exthash_open(e->bp, 0, dir_img, dir_len, &e->ex);
}

static void env_close(env *e)
{
    nx_exthash_close(e->ex);
    nx_bufpool_destroy(e->bp);
    plat_file_close(e->f);
}

/* Stand-in for the M5 checkpoint: write dirty pages in place. */
static void checkpoint(env *e)
{
    if (nx_bufpool_flush_simple(e->bp) != 0)
        fprintf(stderr, "flush failed\n");
    e->checkpoints++;
}

/* The engine's insert loop: split on NEED_SPLIT (exclusive lock), run a
 * checkpoint on NX_BP_FULL, and checkpoint early above 50% dirty. */
static int insert(env *e, const nx_index_entry *ent, uint64_t lsn)
{
    for (int attempt = 0; attempt < 64; attempt++) {
        if (nx_bufpool_dirty_count(e->bp) * 2 > nx_bufpool_frame_count(e->bp))
            checkpoint(e);
        nx_exthash_lock_shared(e->ex);
        int rc = nx_exthash_insert(e->ex, ent, lsn, false);
        nx_exthash_unlock(e->ex);
        if (rc == NX_EXT_NEED_SPLIT) {
            nx_exthash_lock_exclusive(e->ex);
            rc = nx_exthash_split(e->ex, ent->hash);
            nx_exthash_unlock(e->ex);
            if (rc == NX_BP_FULL)
                checkpoint(e);
            else if (rc != 0)
                return rc;
            continue;
        }
        if (rc == NX_BP_FULL) {
            checkpoint(e);
            continue;
        }
        return rc;
    }
    return -1;
}

static int lookup(env *e, const uint8_t hash[32], nx_index_entry *out)
{
    for (;;) {
        nx_exthash_lock_shared(e->ex);
        int rc = nx_exthash_lookup(e->ex, hash, out);
        nx_exthash_unlock(e->ex);
        if (rc != NX_BP_FULL)
            return rc;
        checkpoint(e);
    }
}

static void make_entry(uint64_t i, nx_index_entry *ent)
{
    nx_splitmix s = {i * 0x100000001B3ull + 7};
    for (int k = 0; k < 4; k++) {
        uint64_t z = nx_splitmix_next(&s);
        memcpy(ent->hash + 8 * k, &z, 8);
    }
    ent->container_id = (uint32_t)(i / 800 + 1);
    ent->offset = (uint32_t)(i * 40);
    ent->len = (uint32_t)(2048 + i % 14000);
    ent->refcount = (uint32_t)(i % 7);
    ent->created_at = (uint32_t)(1700000000 + i);
}

static bool entry_eq(const nx_index_entry *a, const nx_index_entry *b)
{
    return memcmp(a->hash, b->hash, 32) == 0 && a->container_id == b->container_id &&
           a->offset == b->offset && a->len == b->len && a->refcount == b->refcount &&
           a->created_at == b->created_at;
}

static void test_one_million_inserts(void)
{
    env e;
    REQUIRE(env_open(&e, "ex_million", 4096, NULL, 0, true) == 0);
    const uint64_t N = 1000000;
    nx_index_entry ent, got;
    uint64_t t0 = plat_now_mono_ms();
    for (uint64_t i = 0; i < N; i++) {
        make_entry(i, &ent);
        if (insert(&e, &ent, i + 1) != 0) {
            CHECK(0);
            break;
        }
    }
    uint64_t t1 = plat_now_mono_ms();
    nx_exthash_stats st;
    nx_exthash_get_stats(e.ex, &st);
    fprintf(stderr,
            "    1M inserts: %llu ms, depth %u, %u buckets, fill %.1f%%, %llu checkpoints\n",
            (unsigned long long)(t1 - t0), st.global_depth, st.buckets, st.avg_fill_percent,
            (unsigned long long)e.checkpoints);
    CHECK_EQ_U64(st.entries, N);
    CHECK(st.global_depth > NX_EXT_INITIAL_DEPTH); /* the directory doubled */
    CHECK(st.buckets > N / NX_BUCKET_CAPACITY);
    CHECK(e.checkpoints > 0); /* pool smaller than the index: no-steal path used */

    /* Duplicates are refused. */
    make_entry(12345, &ent);
    nx_exthash_lock_shared(e.ex);
    CHECK(nx_exthash_insert(e.ex, &ent, N + 1, false) == NX_EXT_EXISTS);
    nx_exthash_unlock(e.ex);

    for (uint64_t i = 0; i < N; i++) {
        make_entry(i, &ent);
        if (lookup(&e, ent.hash, &got) != 0 || !entry_eq(&ent, &got)) {
            CHECK(0);
            break;
        }
    }
    fprintf(stderr, "    1M lookups: %llu ms\n", (unsigned long long)(plat_now_mono_ms() - t1));
    make_entry(N + 5, &ent);
    CHECK(lookup(&e, ent.hash, &got) == NX_EXT_NOT_FOUND);

    /* Persist: pages + dir.bin, then reopen with a cold pool. */
    checkpoint(&e);
    uint8_t *img = NULL;
    size_t img_len = 0;
    REQUIRE(nx_exthash_dir_image(e.ex, &img, &img_len) == 0);
    CHECK(img_len % NX_PAGE_SIZE == 0);
    env_close(&e);
    REQUIRE(env_open(&e, "ex_million", 1024, img, img_len, false) == 0);
    free(img);
    nx_exthash_stats st2;
    nx_exthash_get_stats(e.ex, &st2);
    CHECK_EQ_U64(st2.entries, N);
    CHECK_EQ_U64(st2.global_depth, st.global_depth);
    CHECK_EQ_U64(st2.buckets, st.buckets);
    for (uint64_t i = 0; i < N; i += 997) {
        make_entry(i, &ent);
        CHECK(lookup(&e, ent.hash, &got) == 0 && entry_eq(&ent, &got));
    }
    env_close(&e);
}

static void test_shared_top_bits(void)
{
    /* 600 hashes with identical top 16 bits: splits must go deep and the
     * directory must double many times, yet every entry stays findable. */
    env e;
    REQUIRE(env_open(&e, "ex_collide", 512, NULL, 0, true) == 0);
    nx_index_entry ent, got;
    for (uint64_t i = 0; i < 600; i++) {
        make_entry(i, &ent);
        ent.hash[0] = 0xAB;
        ent.hash[1] = 0xCD;
        REQUIRE(insert(&e, &ent, i + 1) == 0);
    }
    nx_exthash_stats st;
    nx_exthash_get_stats(e.ex, &st);
    fprintf(stderr, "    600 hashes sharing 16 top bits: depth %u, %u buckets\n", st.global_depth,
            st.buckets);
    CHECK(st.global_depth >= 17);
    for (uint64_t i = 0; i < 600; i++) {
        make_entry(i, &ent);
        ent.hash[0] = 0xAB;
        ent.hash[1] = 0xCD;
        CHECK(lookup(&e, ent.hash, &got) == 0 && entry_eq(&ent, &got));
    }
    env_close(&e);
}

/* ---- property test against an in-memory model -------------------------------- */

typedef struct {
    bool used;
    nx_index_entry e;
} slot_t;

#define MODEL_CAP 65536

static slot_t *model_find(slot_t *m, const uint8_t hash[32], bool for_insert)
{
    uint64_t h;
    memcpy(&h, hash + 8, 8);
    for (size_t i = (size_t)(h % MODEL_CAP), n = 0; n < MODEL_CAP; i = (i + 1) % MODEL_CAP, n++) {
        if (!m[i].used)
            return for_insert ? &m[i] : NULL;
        if (memcmp(m[i].e.hash, hash, 32) == 0)
            return &m[i];
    }
    return NULL;
}

/* Linear probing needs tombstones for deletes; rebuild instead (small). */
static void model_remove(slot_t *m, const uint8_t hash[32])
{
    slot_t *s = model_find(m, hash, false);
    if (!s)
        return;
    s->used = false;
    size_t i = (size_t)(s - m);
    for (size_t j = (i + 1) % MODEL_CAP; m[j].used; j = (j + 1) % MODEL_CAP) {
        nx_index_entry tmp = m[j].e;
        m[j].used = false;
        slot_t *t = model_find(m, tmp.hash, true);
        t->used = true;
        t->e = tmp;
    }
}

struct cmp_ctx {
    slot_t *m;
    uint64_t seen;
    int bad;
};

static int compare_cb(const nx_index_entry *e, void *vctx)
{
    struct cmp_ctx *c = vctx;
    slot_t *s = model_find(c->m, e->hash, false);
    if (!s || !entry_eq(&s->e, e))
        c->bad++;
    c->seen++;
    return 0;
}

static void test_property_against_model(void)
{
    env e;
    REQUIRE(env_open(&e, "ex_model", 64, NULL, 0, true) == 0);
    slot_t *m = calloc(MODEL_CAP, sizeof *m);
    REQUIRE(m != NULL);
    nx_splitmix rng = {2026};
    uint64_t lsn = 0, live = 0;
    const uint64_t KEYS = 20000; /* small key space so operations collide */
    for (int op = 0; op < 200000; op++) {
        uint64_t r = nx_splitmix_next(&rng);
        nx_index_entry ent, got;
        make_entry(r % KEYS, &ent);
        slot_t *s = model_find(m, ent.hash, false);
        lsn++;
        switch ((r >> 32) % 5) {
        case 0: { /* insert */
            int rc = insert(&e, &ent, lsn);
            if (s) {
                CHECK(rc == NX_EXT_EXISTS);
            } else {
                CHECK(rc == 0);
                slot_t *t = model_find(m, ent.hash, true);
                t->used = true;
                t->e = ent;
                live++;
            }
            break;
        }
        case 1: { /* lookup */
            int rc = lookup(&e, ent.hash, &got);
            CHECK(s ? (rc == 0 && entry_eq(&s->e, &got)) : rc == NX_EXT_NOT_FOUND);
            break;
        }
        case 2: { /* add_ref, sometimes negative */
            int64_t delta = (int64_t)((r >> 40) % 5) - 2;
            uint32_t nr = 0;
            int rc;
            do {
                nx_exthash_lock_shared(e.ex);
                rc = nx_exthash_add_ref(e.ex, ent.hash, delta, lsn, false, &nr);
                nx_exthash_unlock(e.ex);
                if (rc == NX_BP_FULL)
                    checkpoint(&e);
            } while (rc == NX_BP_FULL);
            if (!s) {
                CHECK(rc == NX_EXT_NOT_FOUND);
            } else if ((int64_t)s->e.refcount + delta < 0) {
                CHECK(rc == NX_EXT_UNDERFLOW);
            } else {
                CHECK(rc == 0);
                s->e.refcount = (uint32_t)((int64_t)s->e.refcount + delta);
                CHECK(nr == s->e.refcount);
            }
            break;
        }
        case 3: { /* move (compaction) */
            uint32_t cid = (uint32_t)(r >> 48), off = (uint32_t)(r >> 16);
            int rc;
            do {
                nx_exthash_lock_shared(e.ex);
                rc = nx_exthash_set_location(e.ex, ent.hash, cid, off, lsn, false);
                nx_exthash_unlock(e.ex);
                if (rc == NX_BP_FULL)
                    checkpoint(&e);
            } while (rc == NX_BP_FULL);
            CHECK(s ? rc == 0 : rc == NX_EXT_NOT_FOUND);
            if (s) {
                s->e.container_id = cid;
                s->e.offset = off;
            }
            break;
        }
        default: { /* remove */
            int rc;
            do {
                nx_exthash_lock_shared(e.ex);
                rc = nx_exthash_remove(e.ex, ent.hash, lsn, false);
                nx_exthash_unlock(e.ex);
                if (rc == NX_BP_FULL)
                    checkpoint(&e);
            } while (rc == NX_BP_FULL);
            CHECK(s ? rc == 0 : rc == NX_EXT_NOT_FOUND);
            if (s) {
                model_remove(m, ent.hash);
                live--;
            }
            break;
        }
        }
        if (nx_test_current_failed)
            break;
    }
    nx_exthash_stats st;
    nx_exthash_get_stats(e.ex, &st);
    CHECK_EQ_U64(st.entries, live);
    struct cmp_ctx cc = {m, 0, 0};
    nx_exthash_lock_shared(e.ex);
    int frc = nx_exthash_for_each(e.ex, compare_cb, &cc);
    nx_exthash_unlock(e.ex);
    CHECK(frc == 0);
    CHECK_EQ_U64(cc.seen, live);
    CHECK(cc.bad == 0);
    fprintf(stderr, "    200k random ops: %llu live entries, %llu checkpoints, all match the model\n",
            (unsigned long long)live, (unsigned long long)e.checkpoints);
    free(m);
    env_close(&e);
}

static void test_redo_skips_applied_changes(void)
{
    env e;
    REQUIRE(env_open(&e, "ex_redo", 64, NULL, 0, true) == 0);
    nx_index_entry ent, got;
    make_entry(1, &ent);
    ent.refcount = 0;
    REQUIRE(insert(&e, &ent, 10) == 0);
    nx_exthash_lock_shared(e.ex);
    /* Replaying an older record against this page is a no-op. */
    CHECK(nx_exthash_insert(e.ex, &ent, 9, true) == NX_EXT_SKIPPED);
    CHECK(nx_exthash_add_ref(e.ex, ent.hash, 1, 10, true, NULL) == NX_EXT_SKIPPED);
    /* A newer record is applied, and then itself skipped on a second replay. */
    CHECK(nx_exthash_add_ref(e.ex, ent.hash, 1, 11, true, NULL) == 0);
    CHECK(nx_exthash_add_ref(e.ex, ent.hash, 1, 11, true, NULL) == NX_EXT_SKIPPED);
    CHECK(nx_exthash_lookup(e.ex, ent.hash, &got) == 0);
    CHECK_EQ_U64(got.refcount, 1);
    nx_exthash_unlock(e.ex);
    env_close(&e);
}

/* Regression (found in M4): redo of an insert that lands in a full bucket.
 * The split must not stamp the pages with the record's LSN, or the retried
 * insert would be skipped as "already applied" and the entry lost. */
static void test_redo_insert_across_split(void)
{
    env e;
    REQUIRE(env_open(&e, "ex_redo_split", 64, NULL, 0, true) == 0);
    /* A fresh index has one bucket page that every slot points at, so the
     * first 78 entries fill it whatever their hashes are. */
    nx_index_entry ent, got;
    uint64_t lsn = 1;
    for (uint64_t i = 0; i < NX_BUCKET_CAPACITY; i++, lsn++) {
        make_entry(i, &ent);
        nx_exthash_lock_shared(e.ex);
        int rc = nx_exthash_insert(e.ex, &ent, lsn, true);
        nx_exthash_unlock(e.ex);
        REQUIRE(rc == 0);
    }
    make_entry(NX_BUCKET_CAPACITY, &ent);
    int rc = -1;
    for (int attempt = 0; attempt < 8; attempt++) {
        nx_exthash_lock_shared(e.ex);
        rc = nx_exthash_insert(e.ex, &ent, lsn, true);
        nx_exthash_unlock(e.ex);
        if (rc != NX_EXT_NEED_SPLIT)
            break;
        nx_exthash_lock_exclusive(e.ex);
        REQUIRE(nx_exthash_split(e.ex, ent.hash) == 0);
        nx_exthash_unlock(e.ex);
    }
    CHECK(rc == 0); /* before the fix: NX_EXT_SKIPPED */
    nx_exthash_lock_shared(e.ex);
    CHECK(nx_exthash_lookup(e.ex, ent.hash, &got) == 0);
    CHECK(entry_eq(&ent, &got));
    for (uint64_t i = 0; i < NX_BUCKET_CAPACITY; i++) {
        nx_index_entry want;
        make_entry(i, &want);
        CHECK(nx_exthash_lookup(e.ex, want.hash, &got) == 0 && entry_eq(&want, &got));
    }
    nx_exthash_unlock(e.ex);
    CHECK(nx_exthash_bucket_count(e.ex) >= 2);
    env_close(&e);
}

static void test_dir_image_rejects_damage(void)
{
    env e;
    REQUIRE(env_open(&e, "ex_dirimg", 64, NULL, 0, true) == 0);
    uint8_t *img = NULL;
    size_t len = 0;
    REQUIRE(nx_exthash_dir_image(e.ex, &img, &len) == 0);
    checkpoint(&e);
    env_close(&e);
    img[20] ^= 1; /* flip a bit in a page id */
    nx_exthash *ex2 = NULL;
    nx_bufpool *bp = NULL;
    REQUIRE(nx_bufpool_create(16, &bp) == 0);
    CHECK(nx_exthash_open(bp, 0, img, len, &ex2) != 0);
    nx_bufpool_destroy(bp);
    free(img);
}

/* ---- concurrency -------------------------------------------------------------- */

struct worker {
    env *e;
    pthread_mutex_t *ckpt_mu;
    uint64_t base;
    int ok;
};

/* Tests only: serialize the stand-in checkpoint (the engine uses the
 * checkpoint rwlock for this). */
static int insert_mt(struct worker *w, const nx_index_entry *ent, uint64_t lsn)
{
    for (int attempt = 0; attempt < 1000; attempt++) {
        nx_exthash_lock_shared(w->e->ex);
        int rc = nx_exthash_insert(w->e->ex, ent, lsn, false);
        nx_exthash_unlock(w->e->ex);
        if (rc == 0)
            return 0;
        if (rc == NX_EXT_NEED_SPLIT) {
            nx_exthash_lock_exclusive(w->e->ex);
            rc = nx_exthash_split(w->e->ex, ent->hash);
            nx_exthash_unlock(w->e->ex);
            if (rc == 0)
                continue; /* split done (by us or another thread): retry the insert */
        }
        if (rc != NX_BP_FULL)
            return rc;
        /* Pool full: write pages out. The exclusive directory lock keeps
         * other threads out while we do, like the real checkpoint lock. */
        nx_exthash_lock_exclusive(w->e->ex);
        pthread_mutex_lock(w->ckpt_mu);
        nx_bufpool_flush_simple(w->e->bp);
        pthread_mutex_unlock(w->ckpt_mu);
        nx_exthash_unlock(w->e->ex);
    }
    return -1;
}

static void *worker_main(void *arg)
{
    struct worker *w = arg;
    w->ok = 1;
    nx_index_entry ent, got;
    for (uint64_t i = 0; i < 20000; i++) {
        make_entry(w->base + i, &ent);
        int irc = insert_mt(w, &ent, w->base + i + 1);
        if (irc != 0) {
            if (w->ok)
                fprintf(stderr, "    insert failed rc=%d i=%llu\n", irc, (unsigned long long)i);
            w->ok = 0;
        }
        /* Look up something written earlier by this thread. */
        make_entry(w->base + i / 2, &ent);
        nx_exthash_lock_shared(w->e->ex);
        int rc = nx_exthash_lookup(w->e->ex, ent.hash, &got);
        nx_exthash_unlock(w->e->ex);
        if (rc != 0 && rc != NX_BP_FULL) {
            if (w->ok)
                fprintf(stderr, "    lookup failed rc=%d i=%llu\n", rc, (unsigned long long)i);
            w->ok = 0;
        }
    }
    return NULL;
}

static void test_concurrent_inserts_and_lookups(void)
{
    env e;
    REQUIRE(env_open(&e, "ex_mt", 2048, NULL, 0, true) == 0);
    pthread_mutex_t ckpt_mu;
    pthread_mutex_init(&ckpt_mu, NULL);
    struct worker w[4];
    pthread_t th[4];
    for (int t = 0; t < 4; t++) {
        w[t] = (struct worker){&e, &ckpt_mu, (uint64_t)t * 1000000, 0};
        REQUIRE(pthread_create(&th[t], NULL, worker_main, &w[t]) == 0);
    }
    for (int t = 0; t < 4; t++) {
        pthread_join(th[t], NULL);
        CHECK(w[t].ok);
    }
    nx_exthash_stats st;
    nx_exthash_get_stats(e.ex, &st);
    CHECK_EQ_U64(st.entries, 80000);
    nx_index_entry ent, got;
    for (int t = 0; t < 4; t++)
        for (uint64_t i = 0; i < 20000; i += 7) {
            make_entry((uint64_t)t * 1000000 + i, &ent);
            CHECK(lookup(&e, ent.hash, &got) == 0 && entry_eq(&ent, &got));
        }
    pthread_mutex_destroy(&ckpt_mu);
    env_close(&e);
}

static const nx_test_case TESTS[] = {
    NX_CASE(test_one_million_inserts),
    NX_CASE(test_shared_top_bits),
    NX_CASE(test_property_against_model),
    NX_CASE(test_redo_skips_applied_changes),
    NX_CASE(test_redo_insert_across_split),
    NX_CASE(test_dir_image_rejects_damage),
    NX_CASE(test_concurrent_inserts_and_lookups),
};

NX_TEST_MAIN(TESTS)
