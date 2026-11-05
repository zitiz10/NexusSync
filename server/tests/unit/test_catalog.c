/*
 * test_catalog.c — users and files tables in slotted pages (SPEC §5, §7.4):
 *   - record encode/decode round trips; malformed records are refused
 *   - many inserts and deletes across pages, holes reused, reopen from disk
 *   - a page whose records were all deleted is compacted and reused
 *   - applying the same change twice is a no-op (logical redo)
 *   - versions: newest (owner, name)
 *   - random operations against an in-memory model through a tiny pool
 */
#include "engine/catalog.h"
#include "nx_test.h"
#include "nx_test_util.h"

typedef struct {
    char dir[600];
    plat_file *fu, *ff;
    nx_bufpool *bp;
    nx_catalog *cat;
} env;

static uint32_t pages_of(plat_file *f)
{
    uint64_t size = 0;
    plat_file_size(f, &size);
    return (uint32_t)(size / NX_PAGE_SIZE);
}

static int env_open(env *e, const char *name, size_t frames, bool fresh)
{
    if (fresh)
        nx_test_tmpdir(name, e->dir, sizeof e->dir);
    char p[700];
    snprintf(p, sizeof p, "%s/users.tbl", e->dir);
    if (plat_file_open(p, PLAT_O_WRITE | PLAT_O_CREATE, &e->fu) != 0)
        return -1;
    snprintf(p, sizeof p, "%s/files.tbl", e->dir);
    if (plat_file_open(p, PLAT_O_WRITE | PLAT_O_CREATE, &e->ff) != 0)
        return -1;
    if (nx_bufpool_create(frames, &e->bp) != 0 || nx_bufpool_attach(e->bp, 1, e->fu) != 0 ||
        nx_bufpool_attach(e->bp, 2, e->ff) != 0)
        return -1;
    return nx_catalog_open(e->bp, 1, pages_of(e->fu), 2, pages_of(e->ff), &e->cat);
}

static void env_close(env *e)
{
    nx_catalog_close(e->cat);
    nx_bufpool_destroy(e->bp);
    plat_file_close(e->fu);
    plat_file_close(e->ff);
}

static void make_file(uint64_t id, uint32_t owner, const char *name, uint32_t version, nx_file *f)
{
    memset(f, 0, sizeof *f);
    f->id = id;
    f->owner_id = owner;
    snprintf(f->name, sizeof f->name, "%s", name);
    f->version = version;
    f->prev_version_id = version > 1 ? id - 1 : 0;
    f->size = id * 1000 + 7;
    f->chunk_count = (uint32_t)(id % 300);
    f->chunker = (uint8_t)(id & 1);
    f->recipe_offset = 16 + id * 100;
    f->recipe_len = 20 + 36 * f->chunk_count;
    for (int i = 0; i < 32; i++)
        f->sha256[i] = (uint8_t)(id * 31 + (uint64_t)i);
    f->bytes_sent = id * 3;
    f->created_at = 1762300000000ull + id;
    f->flags = (uint8_t)(id % 2);
}

static bool file_eq(const nx_file *a, const nx_file *b)
{
    return a->id == b->id && a->owner_id == b->owner_id && strcmp(a->name, b->name) == 0 &&
           a->version == b->version && a->prev_version_id == b->prev_version_id &&
           a->size == b->size && a->chunk_count == b->chunk_count && a->chunker == b->chunker &&
           a->recipe_offset == b->recipe_offset && a->recipe_len == b->recipe_len &&
           memcmp(a->sha256, b->sha256, 32) == 0 && a->bytes_sent == b->bytes_sent &&
           a->created_at == b->created_at && a->flags == b->flags;
}

static void make_user(uint32_t id, const char *username, nx_user *u)
{
    memset(u, 0, sizeof *u);
    u->id = id;
    u->role = id == 1 ? NX_ROLE_ADMIN : NX_ROLE_STUDENT;
    snprintf(u->username, sizeof u->username, "%s", username);
    snprintf(u->display, sizeof u->display, "Student %u", (unsigned)id);
    for (int i = 0; i < 16; i++)
        u->salt[i] = (uint8_t)(id + (uint32_t)i);
    for (int i = 0; i < 32; i++)
        u->pw_hash[i] = (uint8_t)(id * 7 + (uint32_t)i);
    u->pbkdf2_iters = 600000;
    u->created_at = 1762300000000ull + id;
    u->last_seen = u->created_at + 5;
}

static void test_record_round_trip(void)
{
    nx_file f, g;
    make_file(42, 3, "lab 3 – results.csv", 2, &f); /* UTF-8 name */
    uint8_t buf[NX_FILE_REC_MAX];
    size_t n = nx_file_encode(&f, buf);
    REQUIRE(nx_file_decode(buf, n, &g) == 0);
    CHECK(file_eq(&f, &g));
    CHECK(nx_file_decode(buf, n - 1, &g) != 0); /* truncated */
    buf[12] = 0xff;                             /* name length 0x..ff > 255 */
    buf[13] = 0x01;
    CHECK(nx_file_decode(buf, n, &g) != 0);

    /* Longest name fits the maximum record size exactly. */
    char longname[NX_FILENAME_MAX + 1];
    memset(longname, 'a', NX_FILENAME_MAX);
    longname[NX_FILENAME_MAX] = '\0';
    make_file(7, 1, longname, 1, &f);
    CHECK_EQ_U64(nx_file_encode(&f, buf), NX_FILE_REC_MAX);

    nx_user u, v;
    make_user(9, "asha", &u);
    uint8_t ub[NX_USER_REC_MAX];
    memset(ub, 0, sizeof ub);
    size_t un = nx_user_encode(&u, ub);
    REQUIRE(nx_user_decode(ub, un, &v) == 0);
    CHECK(nx_user_decode(ub, NX_USER_REC_MAX, &v) == 0); /* zero padding is accepted */
    CHECK_STR(v.username, "asha");
    CHECK(memcmp(&u.pw_hash, &v.pw_hash, 32) == 0 && v.pbkdf2_iters == 600000);
    ub[NX_USER_REC_MAX - 1] = 1; /* padding must be zero */
    CHECK(nx_user_decode(ub, NX_USER_REC_MAX, &v) != 0);
}

static void test_many_files_and_reopen(void)
{
    env e;
    REQUIRE(env_open(&e, "cat_many", 256, true) == 0);
    const uint64_t N = 3000;
    char name[64];
    for (uint64_t id = 1; id <= N; id++) {
        nx_file f;
        snprintf(name, sizeof name, "file-%04u.csv", (unsigned)(id % 500));
        make_file(id, (uint32_t)(id % 7), name, (uint32_t)(id / 500 + 1), &f);
        REQUIRE(nx_catalog_add_file(e.cat, &f, id) == 0);
    }
    /* Delete every third file, then add new ones: holes get reused. */
    for (uint64_t id = 3; id <= N; id += 3)
        REQUIRE(nx_catalog_delete_file(e.cat, id, N + id) == 0);
    uint32_t pages_before = nx_catalog_page_count(e.cat, true);
    for (uint64_t id = N + 1; id <= N + 900; id++) {
        nx_file f;
        make_file(id, 1, "again.bin", 1, &f);
        REQUIRE(nx_catalog_add_file(e.cat, &f, 2 * N + id) == 0);
    }
    CHECK(nx_catalog_page_count(e.cat, true) <= pages_before + 2);
    CHECK_EQ_U64(nx_catalog_file_count(e.cat), N - N / 3 + 900);
    REQUIRE(nx_bufpool_flush_simple(e.bp) == 0);
    env_close(&e);

    REQUIRE(env_open(&e, "cat_many", 64, false) == 0);
    CHECK_EQ_U64(nx_catalog_file_count(e.cat), N - N / 3 + 900);
    CHECK_EQ_U64(nx_catalog_max_file_id(e.cat), N + 900);
    for (uint64_t id = 1; id <= N + 900; id++) {
        nx_file want, got;
        bool live = id > N || id % 3 != 0;
        bool found = nx_catalog_file_by_id(e.cat, id, &got);
        CHECK(found == live);
        if (found) {
            if (id <= N) {
                snprintf(name, sizeof name, "file-%04u.csv", (unsigned)(id % 500));
                make_file(id, (uint32_t)(id % 7), name, (uint32_t)(id / 500 + 1), &want);
            } else {
                make_file(id, 1, "again.bin", 1, &want);
            }
            CHECK(file_eq(&want, &got));
        }
    }
    env_close(&e);
}

/* A completely full page whose records are all deleted must be compacted
 * back to empty and reused, without growing the table. */
static void test_emptied_page_is_reused(void)
{
    env e;
    REQUIRE(env_open(&e, "cat_empty", 64, true) == 0);
    char longname[NX_FILENAME_MAX + 1];
    memset(longname, 'x', NX_FILENAME_MAX);
    longname[NX_FILENAME_MAX] = '\0';
    /* Fill page 0 completely with maximum-size records (11 fit: 16 + 11 x
     * (4 + 359) = 4009 bytes), delete them all, refill. The first refill
     * insert must compact a page that has no live record left. */
    uint64_t id = 1, lsn = 1;
    for (; id <= 11; id++) {
        nx_file f;
        make_file(id, 1, longname, 1, &f);
        REQUIRE(nx_catalog_add_file(e.cat, &f, lsn++) == 0);
    }
    REQUIRE(nx_catalog_page_count(e.cat, true) == 1);
    for (uint64_t k = 1; k <= 11; k++)
        REQUIRE(nx_catalog_delete_file(e.cat, k, lsn++) == 0);
    for (int round = 0; round < 3; round++) {
        for (uint64_t k = 0; k < 11; k++, id++) {
            nx_file f;
            make_file(id, 1, longname, 1, &f);
            REQUIRE(nx_catalog_add_file(e.cat, &f, lsn++) == 0);
        }
        for (uint64_t k = id - 11; k < id; k++)
            REQUIRE(nx_catalog_delete_file(e.cat, k, lsn++) == 0);
    }
    CHECK_EQ_U64(nx_catalog_page_count(e.cat, true), 1);
    nx_file f;
    make_file(id, 1, "last.txt", 1, &f);
    REQUIRE(nx_catalog_add_file(e.cat, &f, lsn++) == 0);
    REQUIRE(nx_bufpool_flush_simple(e.bp) == 0);
    env_close(&e);
    REQUIRE(env_open(&e, "cat_empty", 64, false) == 0); /* page_sane must accept it */
    nx_file got;
    CHECK(nx_catalog_file_by_id(e.cat, id, &got) && file_eq(&f, &got));
    CHECK_EQ_U64(nx_catalog_file_count(e.cat), 1);
    env_close(&e);
}

static void test_idempotent_apply_and_users(void)
{
    env e;
    REQUIRE(env_open(&e, "cat_idem", 64, true) == 0);
    nx_file f;
    make_file(5, 2, "notes.txt", 1, &f);
    CHECK(nx_catalog_add_file(e.cat, &f, 10) == 0);
    CHECK(nx_catalog_add_file(e.cat, &f, 10) == 1); /* replay: already there */
    CHECK_EQ_U64(nx_catalog_file_count(e.cat), 1);
    CHECK(nx_catalog_set_file_flags(e.cat, 5, NX_FILE_SHARED, 11) == 0);
    nx_file got;
    CHECK(nx_catalog_file_by_id(e.cat, 5, &got) && got.flags == NX_FILE_SHARED);
    CHECK(nx_catalog_delete_file(e.cat, 5, 12) == 0);
    CHECK(nx_catalog_delete_file(e.cat, 5, 12) == 1); /* replay: already gone */
    CHECK(nx_catalog_set_file_flags(e.cat, 5, 0, 13) == 1);

    nx_user u, v;
    for (uint32_t id = 1; id <= 150; id++) {
        char un[16];
        snprintf(un, sizeof un, "user%u", (unsigned)id);
        make_user(id, un, &u);
        REQUIRE(nx_catalog_put_user(e.cat, &u, 100 + id) == 0);
    }
    make_user(7, "user7", &u);
    u.status = NX_STATUS_DISABLED;
    snprintf(u.display, sizeof u.display, "A much longer display name than before");
    REQUIRE(nx_catalog_put_user(e.cat, &u, 400) == 0); /* in-place update */
    REQUIRE(nx_catalog_put_user(e.cat, &u, 400) == 0); /* replay */
    CHECK_EQ_U64(nx_catalog_user_count(e.cat), 150);
    REQUIRE(nx_bufpool_flush_simple(e.bp) == 0);
    env_close(&e);
    REQUIRE(env_open(&e, "cat_idem", 64, false) == 0);
    CHECK_EQ_U64(nx_catalog_user_count(e.cat), 150);
    CHECK_EQ_U64(nx_catalog_max_user_id(e.cat), 150);
    CHECK(nx_catalog_user_by_name(e.cat, "user7", &v));
    CHECK(v.id == 7 && v.status == NX_STATUS_DISABLED);
    CHECK_STR(v.display, "A much longer display name than before");
    CHECK(!nx_catalog_user_by_name(e.cat, "nobody", NULL));
    CHECK(nx_catalog_file_count(e.cat) == 0);
    env_close(&e);
}

static void test_latest_version(void)
{
    env e;
    REQUIRE(env_open(&e, "cat_ver", 64, true) == 0);
    nx_file f, got;
    make_file(1, 1, "a.csv", 1, &f);
    nx_catalog_add_file(e.cat, &f, 1);
    make_file(2, 2, "a.csv", 1, &f); /* other owner */
    nx_catalog_add_file(e.cat, &f, 2);
    make_file(3, 1, "a.csv", 2, &f);
    nx_catalog_add_file(e.cat, &f, 3);
    make_file(4, 1, "b.csv", 1, &f);
    nx_catalog_add_file(e.cat, &f, 4);
    CHECK(nx_catalog_latest_version(e.cat, 1, "a.csv", &got) && got.id == 3 && got.version == 2);
    CHECK(nx_catalog_latest_version(e.cat, 2, "a.csv", &got) && got.id == 2);
    CHECK(!nx_catalog_latest_version(e.cat, 3, "a.csv", &got));
    env_close(&e);
}

/* Random adds/deletes/flag changes through an 8-frame pool. NX_BP_FULL is
 * handled the way the engine does: flush, retry. */
static void test_random_against_model(void)
{
    env e;
    REQUIRE(env_open(&e, "cat_model", 8, true) == 0);
    enum { MAXID = 4000 };
    static uint8_t live[MAXID + 1], flags[MAXID + 1];
    memset(live, 0, sizeof live);
    nx_splitmix rng = {77};
    uint64_t lsn = 1;
    int flushes = 0;
    for (int op = 0; op < 20000; op++) {
        uint64_t id = 1 + nx_splitmix_next(&rng) % MAXID;
        unsigned kind = (unsigned)(nx_splitmix_next(&rng) % 3);
        int rc;
        for (;;) {
            if (kind == 0) {
                nx_file f;
                char name[40];
                snprintf(name, sizeof name, "n%" PRIu64 "-%u", id, (unsigned)(id % 97) * 3);
                make_file(id, 1, name, 1, &f);
                f.flags = 0;
                rc = nx_catalog_add_file(e.cat, &f, lsn);
            } else if (kind == 1) {
                rc = nx_catalog_delete_file(e.cat, id, lsn);
            } else {
                rc = nx_catalog_set_file_flags(e.cat, id, (uint8_t)(op & 3), lsn);
            }
            if (rc != NX_BP_FULL)
                break;
            REQUIRE(nx_bufpool_flush_simple(e.bp) == 0);
            flushes++;
        }
        REQUIRE(rc == 0 || rc == 1);
        bool was = live[id] != 0;
        /* 1 exactly when there was nothing to do: adding a present id, or
         * deleting / flagging a missing one. */
        CHECK(rc == (kind == 0 ? (was ? 1 : 0) : (was ? 0 : 1)));
        if (kind == 0 && !live[id]) {
            live[id] = 1;
            flags[id] = 0;
        } else if (kind == 1) {
            live[id] = 0;
        } else if (kind == 2 && live[id]) {
            flags[id] = (uint8_t)(op & 3);
        }
        lsn++;
    }
    REQUIRE(nx_bufpool_flush_simple(e.bp) == 0);
    env_close(&e);
    REQUIRE(env_open(&e, "cat_model", 8, false) == 0);
    size_t count = 0;
    for (uint64_t id = 1; id <= MAXID; id++) {
        nx_file got;
        bool found = nx_catalog_file_by_id(e.cat, id, &got);
        CHECK(found == (live[id] != 0));
        if (found) {
            count++;
            CHECK(got.flags == flags[id]);
        }
    }
    CHECK_EQ_U64(nx_catalog_file_count(e.cat), count);
    fprintf(stderr, "    20k ops: %zu live files, %u pages, %d pool-full flushes\n", count,
            (unsigned)nx_catalog_page_count(e.cat, true), flushes);
    env_close(&e);
}

static const nx_test_case TESTS[] = {
    NX_CASE(test_record_round_trip),
    NX_CASE(test_many_files_and_reopen),
    NX_CASE(test_emptied_page_is_reused),
    NX_CASE(test_idempotent_apply_and_users),
    NX_CASE(test_latest_version),
    NX_CASE(test_random_against_model),
};

NX_TEST_MAIN(TESTS)
