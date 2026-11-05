/*
 * test_wal.c — the write-ahead log (SPEC §5, §8):
 *   - transactions append, reopen, and are read back in order
 *   - torn tail: the log cut at every byte of the last transaction
 *     recovers to the previous commit, and appending continues cleanly
 *   - a complete transaction without COMMIT is removed
 *   - segment rollover, deleting old segments, reading from a checkpoint
 *   - damage inside an older segment is refused, not silently dropped
 *   - a torn append (crash point) leaves the log recoverable
 */
#include "engine/logrec.h"
#include "engine/wal.h"
#include "nx_test.h"
#include "nx_test_util.h"
#include "util/failpoint.h"

typedef struct {
    uint64_t lsns[4096];
    uint8_t types[4096];
    uint32_t first_word[4096];
    size_t n;
} collected;

static int collect(const nx_wal_rec *r, void *ctx)
{
    collected *c = ctx;
    if (c->n < 4096) {
        c->lsns[c->n] = r->lsn;
        c->types[c->n] = r->type;
        uint32_t v = 0;
        if (r->len >= 4)
            memcpy(&v, r->payload, 4);
        c->first_word[c->n] = v;
        c->n++;
    }
    return 0;
}

/* Writes one transaction BEGIN, k REF_INC records, COMMIT (or no COMMIT). */
static int write_txn(nx_wal *w, size_t k, bool commit, bool sync)
{
    nx_wal_rec recs[64];
    uint8_t payload[64][36];
    uint64_t lsn = nx_wal_next_lsn(w);
    size_t n = 0;
    recs[n++] = (nx_wal_rec){lsn, lsn, NX_REC_BEGIN, 0, NULL};
    for (size_t i = 0; i < k; i++) {
        memset(payload[i], 0, 36);
        uint32_t v = (uint32_t)(lsn + n);
        memcpy(payload[i], &v, 4);
        recs[n] = (nx_wal_rec){lsn + n, lsn, NX_REC_REF_INC, 36, payload[i]};
        n++;
    }
    if (commit) {
        recs[n] = (nx_wal_rec){lsn + n, lsn, NX_REC_COMMIT, 0, NULL};
        n++;
    }
    return nx_wal_append(w, recs, n, sync);
}

static int open_recover(const char *dir, uint64_t seg, uint64_t from, collected *c, nx_wal **w,
                        nx_wal_recovery *rec)
{
    if (nx_wal_open(dir, seg, from + 1, w) != 0)
        return -1;
    if (c)
        c->n = 0;
    if (nx_wal_recover(*w, from, c ? collect : NULL, c, rec) != 0) {
        nx_wal_close(*w);
        *w = NULL;
        return -1;
    }
    return 0;
}

static uint64_t file_size(const char *path)
{
    plat_file *f;
    uint64_t s = 0;
    if (plat_file_open(path, PLAT_O_READ, &f) == 0) {
        plat_file_size(f, &s);
        plat_file_close(f);
    }
    return s;
}

static void test_append_and_read_back(void)
{
    char dir[600];
    nx_test_tmpdir("wal_basic", dir, sizeof dir);
    nx_wal *w;
    nx_wal_recovery rec;
    collected *c = calloc(1, sizeof *c);
    REQUIRE(c && open_recover(dir, NX_WAL_DEFAULT_SEGMENT, 0, c, &w, &rec) == 0);
    CHECK_EQ_U64(c->n, 0);
    CHECK_EQ_U64(nx_wal_next_lsn(w), 1);
    for (int t = 0; t < 20; t++)
        REQUIRE(write_txn(w, (size_t)t % 5, true, true) == 0);
    uint64_t next = nx_wal_next_lsn(w);
    nx_wal_close(w);

    REQUIRE(open_recover(dir, NX_WAL_DEFAULT_SEGMENT, 0, c, &w, &rec) == 0);
    CHECK_EQ_U64(c->n, next - 1);
    CHECK_EQ_U64(rec.bytes_cut, 0);
    CHECK_EQ_U64(rec.last_lsn, next - 1);
    for (size_t i = 0; i < c->n; i++) {
        CHECK_EQ_U64(c->lsns[i], i + 1);
        if (c->types[i] == NX_REC_REF_INC)
            CHECK_EQ_U64(c->first_word[i], c->lsns[i]);
    }
    CHECK_EQ_U64(nx_wal_next_lsn(w), next);
    REQUIRE(write_txn(w, 2, true, true) == 0);
    nx_wal_close(w);

    /* Reading from a checkpoint LSN delivers only later records. */
    REQUIRE(open_recover(dir, NX_WAL_DEFAULT_SEGMENT, 30, c, &w, &rec) == 0);
    CHECK(c->n > 0 && c->lsns[0] == 31);
    CHECK_EQ_U64(c->lsns[c->n - 1], next + 3);
    nx_wal_close(w);
    free(c);
}

static void test_torn_tail_every_byte(void)
{
    char dir[600], seg[700], img_path[700];
    nx_test_tmpdir("wal_torn", dir, sizeof dir);
    snprintf(seg, sizeof seg, "%s/wal.00000001", dir);
    nx_wal *w;
    nx_wal_recovery rec;
    REQUIRE(open_recover(dir, NX_WAL_DEFAULT_SEGMENT, 0, NULL, &w, &rec) == 0);
    for (int t = 0; t < 3; t++)
        REQUIRE(write_txn(w, 3, true, true) == 0); /* 3 x 5 records: lsn 1..15 */
    uint64_t good_end = file_size(seg);
    REQUIRE(write_txn(w, 4, true, true) == 0); /* lsn 16..21 */
    nx_wal_close(w);
    size_t full_len;
    uint8_t *full = nx_read_file(seg, &full_len);
    REQUIRE(full);
    snprintf(img_path, sizeof img_path, "%s", seg);

    collected *c = calloc(1, sizeof *c);
    REQUIRE(c);
    int bad = 0;
    for (size_t cut = good_end; cut < full_len; cut++) {
        plat_file *f;
        REQUIRE(plat_file_open(img_path, PLAT_O_WRITE | PLAT_O_TRUNC, &f) == 0);
        REQUIRE(plat_pwrite_all(f, full, cut, 0) == 0);
        plat_file_close(f);
        if (open_recover(dir, NX_WAL_DEFAULT_SEGMENT, 0, c, &w, &rec) != 0) {
            bad++;
            continue;
        }
        /* Back to the end of transaction 3, whatever byte the cut hit. */
        if (rec.last_lsn != 15 || nx_wal_next_lsn(w) != 16 || file_size(seg) != good_end)
            bad++;
        /* Appending continues, and the new transaction survives a reopen. */
        if (write_txn(w, 1, true, true) != 0)
            bad++;
        nx_wal_close(w);
        if (open_recover(dir, NX_WAL_DEFAULT_SEGMENT, 0, c, &w, &rec) != 0 || rec.last_lsn != 18 ||
            rec.bytes_cut != 0)
            bad++;
        if (w)
            nx_wal_close(w);
    }
    CHECK(bad == 0);
    fprintf(stderr, "    cut at each of %zu byte positions: all recovered to lsn 15\n",
            full_len - (size_t)good_end);

    /* The full file is intact: nothing is cut. */
    plat_file *f;
    REQUIRE(plat_file_open(img_path, PLAT_O_WRITE | PLAT_O_TRUNC, &f) == 0);
    REQUIRE(plat_pwrite_all(f, full, full_len, 0) == 0);
    plat_file_close(f);
    REQUIRE(open_recover(dir, NX_WAL_DEFAULT_SEGMENT, 0, c, &w, &rec) == 0);
    CHECK_EQ_U64(rec.last_lsn, 21);
    /* A complete transaction with no COMMIT at the tail is removed. */
    REQUIRE(write_txn(w, 2, false, true) == 0);
    nx_wal_close(w);
    REQUIRE(open_recover(dir, NX_WAL_DEFAULT_SEGMENT, 0, c, &w, &rec) == 0);
    CHECK_EQ_U64(rec.last_lsn, 21);
    CHECK(rec.bytes_cut > 0);
    CHECK_EQ_U64(c->n, 21 + 3); /* the uncommitted records were still shown to cb */
    CHECK_EQ_U64(nx_wal_next_lsn(w), 22);
    nx_wal_close(w);
    free(full);
    free(c);
}

static void test_segments(void)
{
    char dir[600];
    nx_test_tmpdir("wal_segs", dir, sizeof dir);
    nx_wal *w;
    nx_wal_recovery rec;
    collected *c = calloc(1, sizeof *c);
    REQUIRE(c && open_recover(dir, 4096, 0, NULL, &w, &rec) == 0);
    for (int t = 0; t < 200; t++)
        REQUIRE(write_txn(w, (size_t)(t % 7), true, true) == 0);
    uint64_t next = nx_wal_next_lsn(w);
    nx_wal_stats st;
    nx_wal_get_stats(w, &st);
    CHECK(st.segments > 10);
    fprintf(stderr, "    200 transactions, lsn 1..%" PRIu64 ": %" PRIu32 " segments\n", next - 1,
            st.segments);
    nx_wal_close(w);

    REQUIRE(open_recover(dir, 4096, 0, c, &w, &rec) == 0);
    CHECK_EQ_U64(c->n, next - 1);
    for (size_t i = 0; i < c->n; i++)
        CHECK_EQ_U64(c->lsns[i], i + 1);
    /* A checkpoint at L lets every segment that ends at or before L go. */
    uint64_t L = next / 2;
    REQUIRE(nx_wal_truncate_below(w, L) == 0);
    nx_wal_stats st2;
    nx_wal_get_stats(w, &st2);
    CHECK(st2.segments < st.segments);
    CHECK(st2.bytes < st.bytes);
    nx_wal_close(w);
    REQUIRE(open_recover(dir, 4096, L, c, &w, &rec) == 0);
    CHECK(c->n == next - 1 - L && c->lsns[0] == L + 1);
    REQUIRE(nx_wal_truncate_below(w, next - 1) == 0); /* never deletes the active one */
    nx_wal_get_stats(w, &st2);
    CHECK(st2.segments == 1);
    REQUIRE(write_txn(w, 1, true, true) == 0);
    nx_wal_close(w);
    REQUIRE(open_recover(dir, 4096, next - 1, c, &w, &rec) == 0);
    CHECK(c->n == 3 && c->lsns[0] == next);
    nx_wal_close(w);
    free(c);
}

static void test_damage_in_older_segment(void)
{
    char dir[600], seg[700];
    nx_test_tmpdir("wal_damage", dir, sizeof dir);
    nx_wal *w;
    nx_wal_recovery rec;
    REQUIRE(open_recover(dir, 4096, 0, NULL, &w, &rec) == 0);
    for (int t = 0; t < 100; t++)
        REQUIRE(write_txn(w, 5, true, true) == 0);
    nx_wal_close(w);
    snprintf(seg, sizeof seg, "%s/wal.00000002", dir);
    plat_file *f;
    REQUIRE(plat_file_open(seg, PLAT_O_WRITE, &f) == 0);
    uint8_t b;
    REQUIRE(plat_pread(f, &b, 1, 300) == 1);
    b ^= 0x08;
    REQUIRE(plat_pwrite_all(f, &b, 1, 300) == 0);
    plat_file_close(f);
    CHECK(open_recover(dir, 4096, 0, NULL, &w, &rec) != 0);
}

static void test_torn_append_crash_point(void)
{
    char dir[600];
    nx_test_tmpdir("wal_failpoint", dir, sizeof dir);
    nx_wal *w;
    nx_wal_recovery rec;
    REQUIRE(open_recover(dir, NX_WAL_DEFAULT_SEGMENT, 0, NULL, &w, &rec) == 0);
    REQUIRE(write_txn(w, 3, true, true) == 0);
    nx_fail_arm("wal.append.torn", 0);
    CHECK(write_txn(w, 6, true, true) != 0);
    CHECK(nx_fail_crashed());
    CHECK(write_txn(w, 1, true, true) != 0); /* the log stays closed */
    nx_wal_close(w);
    nx_fail_reset();
    REQUIRE(open_recover(dir, NX_WAL_DEFAULT_SEGMENT, 0, NULL, &w, &rec) == 0);
    CHECK_EQ_U64(rec.last_lsn, 5);
    CHECK(rec.bytes_cut > 0);
    REQUIRE(write_txn(w, 1, true, true) == 0);
    nx_wal_close(w);
}

static const nx_test_case TESTS[] = {
    NX_CASE(test_append_and_read_back),
    NX_CASE(test_torn_tail_every_byte),
    NX_CASE(test_segments),
    NX_CASE(test_damage_in_older_segment),
    NX_CASE(test_torn_append_crash_point),
};

NX_TEST_MAIN(TESTS)
