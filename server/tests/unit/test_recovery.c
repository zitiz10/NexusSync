/*
 * test_recovery.c — crash safety (SPEC §8, §14 M5), in process.
 *
 * A "crash" is a named crash point (util/failpoint.h): from that moment no
 * more bytes are written, and the engine is abandoned without a checkpoint,
 * like a process killed with kill -9. Reopening runs recovery.
 *   - committed work survives a crash with no checkpoint (WAL replay)
 *   - crash between every pair of steps of upload, commit, delete, and
 *     checkpoint: each has a strict expected outcome (the file is there, or
 *     it is not), and fsck (I1–I3) is clean afterwards
 *   - a torn page is repaired from the doublewrite file; without that file
 *     the store refuses to open instead of serving damaged data
 *   - 20 iterations of random work killed at random points, checked
 *     against a model of what was committed (I4)
 */
#include "nx_engine_util.h"

#include <stdatomic.h>
#include "util/failpoint.h"

static void small_cfg(const char *dir, size_t pool, nx_engine_config *cfg)
{
    cfg_for(dir, pool, cfg);
    cfg->wal_segment_size = 64 * 1024; /* many segments: rollover + deletion */
}

static nx_engine *reopen(const char *dir, size_t pool)
{
    nx_fail_reset();
    nx_engine_config cfg;
    small_cfg(dir, pool, &cfg);
    nx_engine *e = NULL;
    return nx_engine_open(&cfg, &e) == 0 ? e : NULL;
}

static nx_engine *fresh(const char *name, char *dir, size_t n, size_t pool)
{
    nx_test_tmpdir(name, dir, n);
    if (nx_engine_create(dir) != 0)
        return NULL;
    return reopen(dir, pool);
}

/* Finds the (single) file named `name`; 0 if none. */
static uint64_t find_file(nx_engine *e, const char *name)
{
    nx_file *fv;
    size_t n;
    uint64_t id = 0;
    if (nx_catalog_files(nx_engine_catalog(e), NULL, NULL, &fv, &n) != 0)
        return 0;
    for (size_t i = 0; i < n; i++)
        if (strcmp(fv[i].name, name) == 0)
            id = fv[i].id;
    free(fv);
    return id;
}

static int upload_named(nx_engine *e, const char *name, const uint8_t *d, size_t len, uint64_t *id)
{
    nx_commit_result res = {0}; /* not filled if the crash comes before the commit */
    upload_info info;
    int rc = upload(e, 1, name, d, len, NX_CHUNKER_CDC, &res, &info);
    free(info.refs);
    if (rc == 0 && id)
        *id = res.file_id;
    nx_commit_result_free(&res);
    return rc;
}

static void test_replay_without_checkpoint(void)
{
    char dir[600];
    nx_engine *e = fresh("rec_replay", dir, sizeof dir, 256);
    REQUIRE(e);
    nx_splitmix rng = {1};
    uint8_t *d[10];
    uint64_t id[10];
    for (int i = 0; i < 10; i++) {
        d[i] = make_content(&rng, 20000 + (size_t)i * 3000);
        char name[32];
        snprintf(name, sizeof name, "f%d.csv", i);
        REQUIRE(upload_named(e, name, d[i], 20000 + (size_t)i * 3000, &id[i]) == 0);
    }
    REQUIRE(nx_engine_delete_file(e, id[3]) == 0);
    nx_user u;
    memset(&u, 0, sizeof u);
    snprintf(u.username, sizeof u.username, "meera");
    REQUIRE(nx_engine_add_user(e, &u) == 0);
    nx_engine_abandon(e); /* killed: nothing checkpointed since open */

    /* Reopen with a progress counter (D-046): it ends equal to the records
     * replayed, which /api/health shows while recovering. */
    nx_fail_reset();
    nx_engine_config pcfg;
    small_cfg(dir, 256, &pcfg);
    _Atomic uint64_t progress = 0;
    pcfg.replay_progress = &progress;
    e = NULL;
    REQUIRE(nx_engine_open(&pcfg, &e) == 0);
    nx_engine_recovery ri;
    nx_engine_last_recovery(e, &ri);
    CHECK(ri.records_replayed > 0 && atomic_load(&progress) == ri.records_replayed);
    fprintf(stderr, "    replayed %" PRIu64 " records in %" PRIu64 " transactions (%" PRIu64 " ms)\n",
            ri.records_replayed, ri.txns_redone, ri.ms);
    CHECK(ri.txns_redone >= 22); /* 10 x (blocks + commit) + delete + user */
    CHECK_EQ_U64(ri.txns_discarded, 0);
    for (int i = 0; i < 10; i++) {
        if (i == 3)
            CHECK(nx_engine_read_file(e, id[i], NULL, NULL) == NX_ENGINE_NOT_FOUND);
        else
            CHECK(read_matches(e, id[i], d[i], 20000 + (size_t)i * 3000));
    }
    CHECK(nx_catalog_user_by_name(nx_engine_catalog(e), "meera", NULL));
    nx_fsck_report r;
    CHECK(fsck_ok(e, &r) == 0);
    /* Ids continue after the replayed ones, including the deleted id 3. */
    uint64_t nid;
    REQUIRE(upload_named(e, "new.csv", d[0], 20000, &nid) == 0);
    CHECK(nid > id[9]);
    /* A second restart has nothing left to replay (the open checkpointed). */
    REQUIRE(nx_engine_close(e) == 0);
    e = reopen(dir, 256);
    REQUIRE(e);
    nx_engine_last_recovery(e, &ri);
    CHECK_EQ_U64(ri.txns_redone, 0);
    REQUIRE(nx_engine_close(e) == 0);
    for (int i = 0; i < 10; i++)
        free(d[i]);
}

/* ---- crash between steps ------------------------------------------------------ */

enum op { OP_UPLOAD, OP_DELETE, OP_CHECKPOINT };
enum expect { GONE, THERE, EITHER };

typedef struct {
    const char *point;
    enum op op;
    enum expect file; /* is the uploaded/deleted file there afterwards? */
    uint32_t skip;
} crash_case;

static const crash_case CASES[] = {
    /* upload: blocks -> container fsync -> CHUNK_ADD txn -> recipe -> commit txn */
    {"container.append.torn", OP_UPLOAD, GONE, 0},
    {"wal.append", OP_UPLOAD, GONE, 0},         /* the CHUNK_ADD batch */
    {"wal.append.torn", OP_UPLOAD, GONE, 1},    /* the commit record, torn */
    {"recipe.append.torn", OP_UPLOAD, GONE, 0},
    {"wal.append", OP_UPLOAD, GONE, 1},         /* recipe written, commit not */
    {"wal.before_fsync", OP_UPLOAD, THERE, 1},  /* written, not fsynced: a process crash keeps it */
    {"txn.after_commit", OP_UPLOAD, THERE, 1},  /* committed, not applied */
    {"txn.mid_apply", OP_UPLOAD, THERE, 1},     /* committed, half applied */
    /* delete: FILE_DEL + REF_DEC */
    {"wal.append.torn", OP_DELETE, THERE, 0},
    {"txn.after_commit", OP_DELETE, GONE, 0},
    {"txn.mid_apply", OP_DELETE, GONE, 0},
    /* checkpoint steps (the file was committed before) */
    {"ckpt.begin", OP_CHECKPOINT, THERE, 0},
    {"doublewrite.torn", OP_CHECKPOINT, THERE, 0},
    {"ckpt.after_doublewrite", OP_CHECKPOINT, THERE, 0},
    {"ckpt.page.torn", OP_CHECKPOINT, THERE, 0},
    {"ckpt.page.torn", OP_CHECKPOINT, THERE, 3},
    {"ckpt.before_superblock", OP_CHECKPOINT, THERE, 0},
    {"superblock.torn", OP_CHECKPOINT, THERE, 0},
    {"ckpt.before_clear", OP_CHECKPOINT, THERE, 0},
};

static void test_crash_between_steps(void)
{
    nx_splitmix rng = {7};
    uint8_t *base[3], *x = make_content(&rng, 60000);
    /* Big enough that the index splits, so a checkpoint has several pages. */
    for (int i = 0; i < 3; i++)
        base[i] = make_content(&rng, 150000);
    int failures = 0;
    for (size_t c = 0; c < sizeof CASES / sizeof CASES[0]; c++) {
        const crash_case *cc = &CASES[c];
        char dir[600], tag[64];
        snprintf(tag, sizeof tag, "rec_step_%02zu", c);
        nx_engine *e = fresh(tag, dir, sizeof dir, 64);
        REQUIRE(e);
        uint64_t bid[3], xid = 0;
        for (int i = 0; i < 3; i++) {
            char name[16];
            snprintf(name, sizeof name, "base%d", i);
            REQUIRE(upload_named(e, name, base[i], 150000, &bid[i]) == 0);
        }
        if (cc->op != OP_UPLOAD)
            REQUIRE(upload_named(e, "x.csv", x, 60000, &xid) == 0);

        nx_fail_arm(cc->point, cc->skip);
        int rc;
        if (cc->op == OP_UPLOAD)
            rc = upload_named(e, "x.csv", x, 60000, NULL);
        else if (cc->op == OP_DELETE)
            rc = nx_engine_delete_file(e, xid);
        else
            rc = nx_engine_checkpoint(e);
        bool crashed = nx_fail_crashed();
        nx_engine_abandon(e);
        nx_fail_reset();
        e = reopen(dir, 64);
        if (!crashed || rc == 0 || !e) {
            fprintf(stderr, "    %-24s skip=%u: crashed=%d rc=%d reopened=%d\n", cc->point,
                    (unsigned)cc->skip, crashed, rc, e != NULL);
            failures++;
            if (e)
                nx_engine_close(e);
            continue;
        }
        nx_engine_recovery ri;
        nx_engine_last_recovery(e, &ri);
        bool ok = true;
        for (int i = 0; i < 3; i++)
            ok = ok && read_matches(e, bid[i], base[i], 150000);
        uint64_t found = find_file(e, "x.csv");
        if (cc->file == THERE)
            ok = ok && found && read_matches(e, found, x, 60000);
        else if (cc->file == GONE)
            ok = ok && !found;
        nx_fsck_report r;
        int fr = fsck_ok(e, &r);
        ok = ok && fr == 0;
        /* A torn page is only survivable thanks to the doublewrite copy. */
        if (strcmp(cc->point, "ckpt.page.torn") == 0 || strcmp(cc->point, "ckpt.after_doublewrite") == 0 ||
            strcmp(cc->point, "ckpt.before_superblock") == 0 || strcmp(cc->point, "superblock.torn") == 0 ||
            strcmp(cc->point, "ckpt.before_clear") == 0)
            ok = ok && ri.pages_repaired > 0;
        if (strcmp(cc->point, "doublewrite.torn") == 0 || strcmp(cc->point, "ckpt.begin") == 0)
            ok = ok && ri.pages_repaired == 0;
        /* A torn superblock: the other copy (the previous checkpoint) is
         * used, so the work since then is replayed from the log. */
        if (strcmp(cc->point, "superblock.torn") == 0)
            ok = ok && ri.txns_redone > 0;
        /* Superblock written, doublewrite not yet cleared: nothing to redo. */
        if (strcmp(cc->point, "ckpt.before_clear") == 0)
            ok = ok && ri.txns_redone == 0;
        fprintf(stderr, "    %-24s %-10s skip=%u: %s (x %s, redone %" PRIu64 ", repaired %" PRIu64
                        " pages, cut %" PRIu64 " B)\n",
                cc->point, cc->op == OP_UPLOAD ? "upload" : cc->op == OP_DELETE ? "delete" : "checkpoint",
                (unsigned)cc->skip, ok ? "ok" : "WRONG", found ? "there" : "gone", ri.txns_redone,
                ri.pages_repaired, ri.wal_bytes_cut);
        if (!ok)
            failures++;
        /* The recovered store keeps working. */
        uint64_t yid;
        if (upload_named(e, "after.csv", x, 60000, &yid) != 0 || !read_matches(e, yid, x, 60000))
            failures++;
        REQUIRE(nx_engine_close(e) == 0);
    }
    CHECK(failures == 0);
    for (int i = 0; i < 3; i++)
        free(base[i]);
    free(x);
}

/* Without the doublewrite copy, a torn page must stop the store from
 * opening (its CRC fails) rather than serve damaged data. */
static void test_torn_page_without_doublewrite(void)
{
    char dir[600];
    nx_engine *e = fresh("rec_no_dw", dir, sizeof dir, 64);
    REQUIRE(e);
    nx_splitmix rng = {9};
    uint8_t *d = make_content(&rng, 200000);
    REQUIRE(upload_named(e, "big.csv", d, 200000, NULL) == 0);
    nx_fail_arm("ckpt.page.torn", 0);
    CHECK(nx_engine_checkpoint(e) != 0);
    nx_engine_abandon(e);
    nx_fail_reset();
    char dw[700];
    snprintf(dw, sizeof dw, "%s/doublewrite.bin", dir);
    REQUIRE(plat_remove(dw) == 0);
    e = reopen(dir, 64);
    CHECK(e == NULL);
    if (e)
        nx_engine_close(e);
    free(d);
}

/* ---- crash loop ---------------------------------------------------------------- */

static const char *const POINTS[] = {
    "container.append.torn", "recipe.append.torn", "wal.append",        "wal.append.torn",
    "wal.before_fsync",      "txn.after_commit",   "txn.mid_apply",     "ckpt.begin",
    "doublewrite.torn",      "ckpt.after_doublewrite", "ckpt.page.torn", "ckpt.before_superblock",
    "superblock.torn",       "ckpt.before_clear",
};

typedef struct {
    uint64_t id;
    char name[32];
    uint8_t *data;
    size_t len;
} mfile;

typedef struct {
    mfile f[600];
    size_t n;
    char users[64][24];
    size_t nu;
} model;

static void model_remove(model *m, size_t i)
{
    free(m->f[i].data);
    m->f[i] = m->f[--m->n];
}

static void test_crash_loop_20(void)
{
    char dir[600];
    nx_engine *e = fresh("rec_loop", dir, sizeof dir, 32);
    REQUIRE(e);
    static model m;
    memset(&m, 0, sizeof m);
    nx_splitmix rng = {2026};
    int name_seq = 0, problems = 0;
    uint64_t total_ops = 0, total_redone = 0, total_repaired = 0, total_cut = 0;
    char hits[64][24];
    size_t nhits = 0;
    for (int iter = 0; iter < 20; iter++) {
        /* In-flight operation when the crash happened. */
        enum { NONE, UP, DEL, USER } inflight = NONE;
        mfile pending = {0};
        char pending_user[24] = "";
        size_t del_index = 0;

        const char *point = POINTS[nx_splitmix_next(&rng) % (sizeof POINTS / sizeof POINTS[0])];
        nx_fail_arm(point, (uint32_t)(nx_splitmix_next(&rng) % 25));
        for (int op = 0; op < 30 && !nx_fail_crashed(); op++) {
            unsigned k = (unsigned)(nx_splitmix_next(&rng) % 20);
            total_ops++;
            if (k < 13 || m.n < 3) { /* upload: new, duplicate, or edited copy */
                mfile nf = {0};
                snprintf(nf.name, sizeof nf.name, "n%d.csv", name_seq++);
                if (m.n == 0 || k < 6) {
                    nf.len = (size_t)(nx_splitmix_next(&rng) % 40000);
                    nf.data = make_content(&rng, nf.len);
                } else if (k < 8) {
                    const mfile *s = &m.f[nx_splitmix_next(&rng) % m.n];
                    nf.len = s->len;
                    nf.data = malloc(s->len ? s->len : 1);
                    memcpy(nf.data, s->data, s->len);
                } else {
                    const mfile *s = &m.f[nx_splitmix_next(&rng) % m.n];
                    nf.data = edit_copy(&rng, s->data, s->len, &nf.len);
                }
                if (upload_named(e, nf.name, nf.data, nf.len, &nf.id) == 0 && m.n < 600) {
                    m.f[m.n++] = nf;
                } else if (nx_fail_crashed()) {
                    inflight = UP;
                    pending = nf;
                } else {
                    problems++;
                    free(nf.data);
                }
            } else if (k < 16) { /* delete */
                size_t i = nx_splitmix_next(&rng) % m.n;
                if (nx_engine_delete_file(e, m.f[i].id) == 0)
                    model_remove(&m, i);
                else if (nx_fail_crashed()) {
                    inflight = DEL;
                    del_index = i;
                } else
                    problems++;
            } else if (k < 17 && m.nu < 64) { /* user */
                nx_user u;
                memset(&u, 0, sizeof u);
                snprintf(u.username, sizeof u.username, "u%d-%d", iter, op);
                if (nx_engine_add_user(e, &u) == 0)
                    snprintf(m.users[m.nu++], 24, "%s", u.username);
                else if (nx_fail_crashed()) {
                    inflight = USER;
                    snprintf(pending_user, sizeof pending_user, "%s", u.username);
                } else
                    problems++;
            } else if (nx_engine_checkpoint(e) != 0 && !nx_fail_crashed()) {
                problems++;
            }
        }
        if (nx_fail_crashed() && nhits < 64)
            snprintf(hits[nhits++], 24, "%s", nx_fail_crashed_at());
        /* Killed here, whether or not the crash point fired. */
        nx_engine_abandon(e);
        e = reopen(dir, 32);
        if (!e) {
            fprintf(stderr, "    iteration %d: reopen FAILED after crash at %s\n", iter, point);
            problems++;
            break;
        }
        nx_engine_recovery ri;
        nx_engine_last_recovery(e, &ri);
        total_redone += ri.txns_redone;
        total_repaired += ri.pages_repaired;
        total_cut += ri.wal_bytes_cut;

        /* Resolve the in-flight operation: either outcome is allowed (it
         * depends on whether its COMMIT reached the log), but nothing else. */
        if (inflight == UP) {
            uint64_t id = find_file(e, pending.name);
            if (id) {
                pending.id = id;
                if (m.n < 600)
                    m.f[m.n++] = pending;
                else
                    free(pending.data);
            } else {
                free(pending.data);
            }
        } else if (inflight == DEL) {
            if (!nx_catalog_file_by_id(nx_engine_catalog(e), m.f[del_index].id, NULL))
                model_remove(&m, del_index);
        } else if (inflight == USER) {
            if (nx_catalog_user_by_name(nx_engine_catalog(e), pending_user, NULL))
                snprintf(m.users[m.nu++], 24, "%s", pending_user);
        }

        /* The recovered state equals the committed state (I4). */
        bool ok = nx_catalog_file_count(nx_engine_catalog(e)) == m.n;
        for (size_t i = 0; i < m.n; i++)
            ok = read_matches(e, m.f[i].id, m.f[i].data, m.f[i].len) && ok;
        for (size_t i = 0; i < m.nu; i++)
            ok = nx_catalog_user_by_name(nx_engine_catalog(e), m.users[i], NULL) && ok;
        nx_fsck_report r;
        ok = fsck_ok(e, &r) == 0 && ok;
        if (!ok) {
            fprintf(stderr, "    iteration %d: state WRONG after crash at %s\n", iter, point);
            problems++;
        }
    }
    CHECK(problems == 0);
    fprintf(stderr,
            "    20 kills, %" PRIu64 " operations, %zu crash points fired; recovery redid %" PRIu64
            " transactions, repaired %" PRIu64 " pages, cut %" PRIu64 " B; %zu files at the end\n",
            total_ops, nhits, total_redone, total_repaired, total_cut, m.n);
    fprintf(stderr, "    fired:");
    for (size_t i = 0; i < nhits; i++)
        fprintf(stderr, " %s", hits[i]);
    fprintf(stderr, "\n");
    CHECK(nhits >= 10); /* most iterations really crashed mid-operation */
    if (e)
        REQUIRE(nx_engine_close(e) == 0);
    for (size_t i = 0; i < m.n; i++)
        free(m.f[i].data);
}

static const nx_test_case TESTS[] = {
    NX_CASE(test_replay_without_checkpoint),
    NX_CASE(test_crash_between_steps),
    NX_CASE(test_torn_page_without_doublewrite),
    NX_CASE(test_crash_loop_20),
};

NX_TEST_MAIN(TESTS)
