/* test_container.c — append-only block containers (SPEC §5, §7.1). */
#include "crypto/sha256.h"
#include "engine/container.h"
#include "nx_test.h"
#include "nx_test_util.h"
#include "util/bytes.h"

#include <inttypes.h>
#include <pthread.h>

#define SMALL_CONTAINER (64u * 1024u)

/* Deterministic block i: length 100..2099 bytes of SplitMix64 data. */
static uint8_t *make_block(uint64_t i, uint32_t *len, uint8_t hash[32])
{
    *len = (uint32_t)(100 + (i * 7919) % 2000);
    uint8_t *b = nx_splitmix_bytes(i + 1, *len);
    nx_sha256(b, *len, hash);
    return b;
}

static void test_append_read_roundtrip(void)
{
    char dir[600];
    nx_test_tmpdir("ct_roundtrip", dir, sizeof dir);
    nx_containers *c = NULL;
    REQUIRE(nx_containers_open(dir, 4u << 20, &c) == 0);
    CHECK_EQ_U64(nx_containers_active_id(c), 1);
    nx_block_loc locs[50];
    for (uint64_t i = 0; i < 50; i++) {
        uint32_t len;
        uint8_t h[32];
        uint8_t *b = make_block(i, &len, h);
        CHECK(nx_containers_append(c, h, b, len, &locs[i]) == 0);
        free(b);
    }
    CHECK(nx_containers_sync(c) == 0);
    uint8_t buf[4096], stored[32];
    for (uint64_t i = 0; i < 50; i++) {
        uint32_t len;
        uint8_t h[32];
        uint8_t *b = make_block(i, &len, h);
        bool corrupt = true;
        CHECK(nx_containers_read(c, &locs[i], buf, sizeof buf, stored, &corrupt) == 0);
        CHECK(!corrupt);
        CHECK(memcmp(buf, b, len) == 0);
        CHECK(memcmp(stored, h, 32) == 0);
        free(b);
    }
    /* A wrong length in the location is refused, not silently read. */
    nx_block_loc bad = locs[3];
    bad.len += 1;
    CHECK(nx_containers_read(c, &bad, buf, sizeof buf, NULL, NULL) != 0);
    nx_containers_close(c);
}

static void test_rotation_and_reopen(void)
{
    char dir[600];
    nx_test_tmpdir("ct_rotate", dir, sizeof dir);
    nx_containers *c = NULL;
    REQUIRE(nx_containers_open(dir, SMALL_CONTAINER, &c) == 0);
    nx_block_loc locs[200];
    for (uint64_t i = 0; i < 200; i++) {
        uint32_t len;
        uint8_t h[32];
        uint8_t *b = make_block(i, &len, h);
        CHECK(nx_containers_append(c, h, b, len, &locs[i]) == 0);
        free(b);
    }
    CHECK(nx_containers_sync(c) == 0);
    uint32_t active = nx_containers_active_id(c);
    CHECK(active > 3); /* ~220 KB of records in 64 KB containers */
    /* No container exceeds the seal threshold. */
    uint32_t *ids = NULL;
    size_t n = 0;
    REQUIRE(nx_containers_list(c, &ids, &n) == 0);
    CHECK_EQ_U64(n, active);
    for (size_t k = 0; k < n; k++) {
        uint64_t size = 0;
        CHECK(nx_containers_file_size(c, ids[k], &size) == 0);
        CHECK(size <= SMALL_CONTAINER);
    }
    free(ids);
    nx_containers_close(c);

    /* Reopen: the highest container is active again and appends continue
     * at its end; every old record is still readable. */
    REQUIRE(nx_containers_open(dir, SMALL_CONTAINER, &c) == 0);
    CHECK_EQ_U64(nx_containers_active_id(c), active);
    uint32_t len;
    uint8_t h[32], buf[4096];
    uint8_t *b = make_block(999, &len, h);
    nx_block_loc extra;
    CHECK(nx_containers_append(c, h, b, len, &extra) == 0);
    free(b);
    CHECK(extra.container_id >= active);
    CHECK(nx_containers_sync(c) == 0);
    for (uint64_t i = 0; i < 200; i += 13)
        CHECK(nx_containers_read(c, &locs[i], buf, sizeof buf, NULL, NULL) == 0);
    nx_containers_close(c);
}

static void append_garbage(const char *dir, uint32_t id, const uint8_t *bytes, size_t n)
{
    char path[700];
    snprintf(path, sizeof path, "%s/c%08" PRIu32 ".dat", dir, id);
    plat_file *f = NULL;
    if (plat_file_open(path, PLAT_O_WRITE, &f) != 0)
        return;
    uint64_t size = 0;
    plat_file_size(f, &size);
    plat_pwrite_all(f, bytes, n, size);
    plat_file_close(f);
}

struct count_ctx {
    int records, bad;
};

static int count_cb(const nx_block_loc *loc, const uint8_t hash[32], bool crc_ok,
                    const uint8_t *data, void *vctx)
{
    struct count_ctx *cc = vctx;
    (void)loc;
    (void)hash;
    (void)data;
    cc->records++;
    cc->bad += !crc_ok;
    return 0;
}

static void test_torn_tail_starts_new_container(void)
{
    char dir[600];
    nx_test_tmpdir("ct_torn", dir, sizeof dir);
    nx_containers *c = NULL;
    REQUIRE(nx_containers_open(dir, 4u << 20, &c) == 0);
    nx_block_loc locs[10];
    for (uint64_t i = 0; i < 10; i++) {
        uint32_t len;
        uint8_t h[32];
        uint8_t *b = make_block(i, &len, h);
        CHECK(nx_containers_append(c, h, b, len, &locs[i]) == 0);
        free(b);
    }
    CHECK(nx_containers_sync(c) == 0);
    nx_containers_close(c);

    /* Simulate a crash in the middle of an append: a record header saying
     * 5000 bytes, followed by only 100 bytes. */
    uint8_t torn[140];
    memset(torn, 0xAB, sizeof torn);
    nx_put_u32(torn + 32, 5000);
    append_garbage(dir, 1, torn, sizeof torn);

    REQUIRE(nx_containers_open(dir, 4u << 20, &c) == 0);
    CHECK_EQ_U64(nx_containers_active_id(c), 2); /* did not append after garbage */
    uint8_t buf[4096];
    for (int i = 0; i < 10; i++)
        CHECK(nx_containers_read(c, &locs[i], buf, sizeof buf, NULL, NULL) == 0);
    struct count_ctx cc = {0, 0};
    uint64_t trailing = 0;
    CHECK(nx_containers_scan(c, 1, false, count_cb, &cc, &trailing) == 0);
    CHECK(cc.records == 10 && cc.bad == 0);
    CHECK_EQ_U64(trailing, sizeof torn);
    nx_containers_close(c);

    /* Reopening again is stable: container 2 is clean and stays active. */
    REQUIRE(nx_containers_open(dir, 4u << 20, &c) == 0);
    CHECK_EQ_U64(nx_containers_active_id(c), 2);
    nx_containers_close(c);
}

static void test_incomplete_header_is_rewritten(void)
{
    char dir[600], path[700];
    nx_test_tmpdir("ct_header", dir, sizeof dir);
    nx_containers *c = NULL;
    REQUIRE(nx_containers_open(dir, 4u << 20, &c) == 0);
    nx_containers_close(c);
    /* A crash right after creating container 2: only 10 header bytes. */
    snprintf(path, sizeof path, "%s/c00000002.dat", dir);
    plat_file *f = NULL;
    REQUIRE(plat_file_open(path, PLAT_O_WRITE | PLAT_O_CREATE, &f) == 0);
    CHECK(plat_pwrite_all(f, "NXSC\x01\x00\x00", 7, 0) == 0);
    plat_file_close(f);
    REQUIRE(nx_containers_open(dir, 4u << 20, &c) == 0);
    CHECK_EQ_U64(nx_containers_active_id(c), 2);
    uint8_t h[32] = {1}, data[10] = {2};
    nx_block_loc loc;
    CHECK(nx_containers_append(c, h, data, sizeof data, &loc) == 0);
    CHECK(loc.container_id == 2 && loc.offset == NX_CONTAINER_HEADER);
    CHECK(nx_containers_sync(c) == 0);
    nx_containers_close(c);
}

static void test_corruption_detected(void)
{
    char dir[600], path[700];
    nx_test_tmpdir("ct_corrupt", dir, sizeof dir);
    nx_containers *c = NULL;
    REQUIRE(nx_containers_open(dir, 4u << 20, &c) == 0);
    nx_block_loc locs[3];
    for (uint64_t i = 0; i < 3; i++) {
        uint32_t len;
        uint8_t h[32];
        uint8_t *b = make_block(i, &len, h);
        CHECK(nx_containers_append(c, h, b, len, &locs[i]) == 0);
        free(b);
    }
    CHECK(nx_containers_sync(c) == 0);
    nx_containers_close(c);

    /* Flip one byte in the middle block's data. */
    snprintf(path, sizeof path, "%s/c00000001.dat", dir);
    plat_file *f = NULL;
    REQUIRE(plat_file_open(path, PLAT_O_WRITE, &f) == 0);
    uint64_t pos = (uint64_t)locs[1].offset + NX_RECORD_HEADER + 17;
    uint8_t byte = 0;
    CHECK(plat_pread(f, &byte, 1, pos) == 1);
    byte ^= 0x01;
    CHECK(plat_pwrite_all(f, &byte, 1, pos) == 0);
    plat_file_close(f);

    REQUIRE(nx_containers_open(dir, 4u << 20, &c) == 0);
    uint8_t buf[4096];
    bool corrupt = false;
    CHECK(nx_containers_read(c, &locs[1], buf, sizeof buf, NULL, &corrupt) != 0);
    CHECK(corrupt);
    CHECK(nx_containers_read(c, &locs[0], buf, sizeof buf, NULL, &corrupt) == 0);
    CHECK(nx_containers_read(c, &locs[2], buf, sizeof buf, NULL, &corrupt) == 0);
    /* The scan keeps going past a damaged record in the middle and flags it. */
    struct count_ctx cc = {0, 0};
    CHECK(nx_containers_scan(c, 1, false, count_cb, &cc, NULL) == 0);
    CHECK(cc.records == 3 && cc.bad == 1);
    nx_containers_close(c);
}

static void test_drop(void)
{
    char dir[600];
    nx_test_tmpdir("ct_drop", dir, sizeof dir);
    nx_containers *c = NULL;
    REQUIRE(nx_containers_open(dir, 4u << 20, &c) == 0);
    uint8_t h[32] = {9}, data[100] = {7}, buf[100];
    nx_block_loc loc;
    CHECK(nx_containers_append(c, h, data, sizeof data, &loc) == 0);
    CHECK(nx_containers_sync(c) == 0);
    CHECK(nx_containers_read(c, &loc, buf, sizeof buf, NULL, NULL) == 0); /* caches a handle */
    CHECK(nx_containers_drop(c, 1) != 0); /* active: refused */
    uint32_t nid = 0;
    CHECK(nx_containers_rotate(c, &nid) == 0);
    CHECK_EQ_U64(nid, 2);
    CHECK(nx_containers_drop(c, 1) == 0);
    CHECK(nx_containers_read(c, &loc, buf, sizeof buf, NULL, NULL) != 0);
    nx_containers_close(c);
}

struct writer {
    nx_containers *c;
    uint64_t base;
    nx_block_loc locs[300];
    int ok;
};

static void *writer_main(void *arg)
{
    struct writer *w = arg;
    w->ok = 1;
    for (uint64_t i = 0; i < 300; i++) {
        uint32_t len;
        uint8_t h[32];
        uint8_t *b = make_block(w->base + i, &len, h);
        if (nx_containers_append(w->c, h, b, len, &w->locs[i]) != 0)
            w->ok = 0;
        free(b);
        if (i % 50 == 0 && nx_containers_sync(w->c) != 0)
            w->ok = 0;
    }
    return NULL;
}

static void test_concurrent_appends(void)
{
    char dir[600];
    nx_test_tmpdir("ct_concurrent", dir, sizeof dir);
    nx_containers *c = NULL;
    REQUIRE(nx_containers_open(dir, SMALL_CONTAINER, &c) == 0);
    struct writer w[4];
    pthread_t th[4];
    for (int t = 0; t < 4; t++) {
        w[t].c = c;
        w[t].base = (uint64_t)t * 10000;
        REQUIRE(pthread_create(&th[t], NULL, writer_main, &w[t]) == 0);
    }
    for (int t = 0; t < 4; t++)
        pthread_join(th[t], NULL);
    CHECK(nx_containers_sync(c) == 0);
    uint8_t buf[4096];
    for (int t = 0; t < 4; t++) {
        CHECK(w[t].ok);
        for (uint64_t i = 0; i < 300; i++) {
            uint32_t len;
            uint8_t h[32], stored[32];
            uint8_t *b = make_block(w[t].base + i, &len, h);
            CHECK(nx_containers_read(c, &w[t].locs[i], buf, sizeof buf, stored, NULL) == 0);
            CHECK(memcmp(buf, b, len) == 0 && memcmp(stored, h, 32) == 0);
            free(b);
        }
    }
    nx_containers_close(c);
}

static const nx_test_case TESTS[] = {
    NX_CASE(test_append_read_roundtrip),
    NX_CASE(test_rotation_and_reopen),
    NX_CASE(test_torn_tail_starts_new_container),
    NX_CASE(test_incomplete_header_is_rewritten),
    NX_CASE(test_corruption_detected),
    NX_CASE(test_drop),
    NX_CASE(test_concurrent_appends),
};

NX_TEST_MAIN(TESTS)
