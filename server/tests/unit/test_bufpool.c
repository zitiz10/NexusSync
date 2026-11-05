/* test_bufpool.c — buffer pool: pin/unpin, Clock replacement, no-steal,
 * page checksums (SPEC §7.3). */
#include "engine/bufpool.h"
#include "nx_test.h"
#include "nx_test_util.h"

static plat_file *open_pagefile(const char *dir, const char *name)
{
    char path[700];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    plat_file *f = NULL;
    if (plat_file_open(path, PLAT_O_WRITE | PLAT_O_CREATE, &f) != 0)
        return NULL;
    return f;
}

/* Writes a recognizable pattern for (page, version) into a pinned page. */
static void fill(nx_bufpool *bp, uint32_t page, uint8_t version)
{
    nx_frame *fr;
    if (nx_bufpool_pin(bp, 0, page, NX_PIN_NEW, &fr) != 0)
        return;
    nx_frame_wrlock(fr);
    uint8_t *d = nx_frame_data(fr);
    for (size_t i = 16; i < NX_PAGE_SIZE; i++)
        d[i] = (uint8_t)(page * 31 + version + i);
    nx_bufpool_mark_dirty(bp, fr, 100 + page);
    nx_frame_unlock(fr);
    nx_bufpool_unpin(bp, fr);
}

static bool matches(nx_bufpool *bp, uint32_t page, uint8_t version)
{
    nx_frame *fr;
    if (nx_bufpool_pin(bp, 0, page, NX_PIN_READ, &fr) != 0)
        return false;
    nx_frame_rdlock(fr);
    const uint8_t *d = nx_frame_data(fr);
    bool ok = nx_page_lsn(d) == 100 + page;
    for (size_t i = 16; i < NX_PAGE_SIZE && ok; i++)
        ok = d[i] == (uint8_t)(page * 31 + version + i);
    nx_frame_unlock(fr);
    nx_bufpool_unpin(bp, fr);
    return ok;
}

static void test_write_flush_reread(void)
{
    char dir[600];
    nx_test_tmpdir("bp_basic", dir, sizeof dir);
    plat_file *f = open_pagefile(dir, "pages.bin");
    REQUIRE(f != NULL);
    nx_bufpool *bp = NULL;
    REQUIRE(nx_bufpool_create(16, &bp) == 0);
    REQUIRE(nx_bufpool_attach(bp, 0, f) == 0);
    for (uint32_t p = 0; p < 10; p++)
        fill(bp, p, 1);
    CHECK_EQ_U64(nx_bufpool_dirty_count(bp), 10);
    CHECK(nx_bufpool_flush_simple(bp) == 0);
    CHECK_EQ_U64(nx_bufpool_dirty_count(bp), 0);
    /* Forget the cache: the next reads come from disk and pass the CRC. */
    nx_bufpool_drop_file(bp, 0);
    for (uint32_t p = 0; p < 10; p++)
        CHECK(matches(bp, p, 1));
    nx_bufpool_stats st;
    nx_bufpool_get_stats(bp, &st);
    CHECK_EQ_U64(st.misses, 10);
    for (uint32_t p = 0; p < 10; p++)
        CHECK(matches(bp, p, 1));
    nx_bufpool_get_stats(bp, &st);
    CHECK_EQ_U64(st.hits, 10);
    /* A page never written reads as zeros (beyond end of file). */
    nx_frame *fr;
    REQUIRE(nx_bufpool_pin(bp, 0, 500, NX_PIN_READ, &fr) == 0);
    CHECK(nx_page_lsn(nx_frame_data(fr)) == 0);
    nx_bufpool_unpin(bp, fr);
    nx_bufpool_destroy(bp);
    plat_file_close(f);
}

static void test_no_steal(void)
{
    char dir[600];
    nx_test_tmpdir("bp_nosteal", dir, sizeof dir);
    plat_file *f = open_pagefile(dir, "pages.bin");
    REQUIRE(f != NULL);
    nx_bufpool *bp = NULL;
    REQUIRE(nx_bufpool_create(8, &bp) == 0);
    REQUIRE(nx_bufpool_attach(bp, 0, f) == 0);
    for (uint32_t p = 0; p < 8; p++)
        fill(bp, p, 2);
    /* Every frame is dirty: the pool must refuse rather than write a dirty
     * page outside a checkpoint. */
    nx_frame *fr;
    CHECK(nx_bufpool_pin(bp, 0, 8, NX_PIN_READ, &fr) == NX_BP_FULL);
    uint64_t size = 1;
    CHECK(plat_file_size(f, &size) == 0);
    CHECK_EQ_U64(size, 0); /* nothing reached the disk */
    CHECK(nx_bufpool_flush_simple(bp) == 0);
    CHECK(nx_bufpool_pin(bp, 0, 8, NX_PIN_READ, &fr) == 0);
    nx_bufpool_unpin(bp, fr);
    for (uint32_t p = 0; p < 8; p++)
        CHECK(matches(bp, p, 2));
    nx_bufpool_destroy(bp);
    plat_file_close(f);
}

static void test_pinned_frames_stay(void)
{
    char dir[600];
    nx_test_tmpdir("bp_pinned", dir, sizeof dir);
    plat_file *f = open_pagefile(dir, "pages.bin");
    REQUIRE(f != NULL);
    nx_bufpool *bp = NULL;
    REQUIRE(nx_bufpool_create(8, &bp) == 0);
    REQUIRE(nx_bufpool_attach(bp, 0, f) == 0);
    nx_frame *held[8], *fr;
    for (uint32_t p = 0; p < 8; p++)
        REQUIRE(nx_bufpool_pin(bp, 0, p, NX_PIN_READ, &held[p]) == 0);
    CHECK(nx_bufpool_pin(bp, 0, 99, NX_PIN_READ, &fr) == NX_BP_FULL);
    nx_bufpool_unpin(bp, held[5]);
    CHECK(nx_bufpool_pin(bp, 0, 99, NX_PIN_READ, &fr) == 0);
    CHECK(nx_frame_page_no(fr) == 99);
    CHECK(fr == held[5]); /* the only unpinned frame was reused */
    nx_bufpool_unpin(bp, fr);
    for (uint32_t p = 0; p < 8; p++)
        if (p != 5)
            nx_bufpool_unpin(bp, held[p]);
    nx_bufpool_destroy(bp);
    plat_file_close(f);
}

static void test_clock_second_chance(void)
{
    char dir[600];
    nx_test_tmpdir("bp_clock", dir, sizeof dir);
    plat_file *f = open_pagefile(dir, "pages.bin");
    REQUIRE(f != NULL);
    nx_bufpool *bp = NULL;
    REQUIRE(nx_bufpool_create(8, &bp) == 0);
    REQUIRE(nx_bufpool_attach(bp, 0, f) == 0);
    nx_frame *fr;
    for (uint32_t p = 0; p < 8; p++) { /* fills frames 0..7 in order */
        REQUIRE(nx_bufpool_pin(bp, 0, p, NX_PIN_READ, &fr) == 0);
        nx_bufpool_unpin(bp, fr);
    }
    /* All reference bits set: page 8 makes the hand clear them all and come
     * back to frame 0, so page 0 is evicted. */
    REQUIRE(nx_bufpool_pin(bp, 0, 8, NX_PIN_READ, &fr) == 0);
    nx_bufpool_unpin(bp, fr);
    /* Touch page 2 (sets its bit again), then load page 9. The hand is at
     * frame 1 with bit clear, so page 1 goes, not page 2. */
    REQUIRE(nx_bufpool_pin(bp, 0, 2, NX_PIN_READ, &fr) == 0);
    nx_bufpool_unpin(bp, fr);
    REQUIRE(nx_bufpool_pin(bp, 0, 9, NX_PIN_READ, &fr) == 0);
    nx_bufpool_unpin(bp, fr);
    /* Page 2 survived (hit); page 1 was evicted (miss). Page 3 next. */
    nx_bufpool_stats before, after;
    nx_bufpool_get_stats(bp, &before);
    REQUIRE(nx_bufpool_pin(bp, 0, 2, NX_PIN_READ, &fr) == 0);
    nx_bufpool_unpin(bp, fr);
    nx_bufpool_get_stats(bp, &after);
    CHECK_EQ_U64(after.hits, before.hits + 1);
    REQUIRE(nx_bufpool_pin(bp, 0, 1, NX_PIN_READ, &fr) == 0);
    nx_bufpool_unpin(bp, fr);
    nx_bufpool_get_stats(bp, &after);
    CHECK_EQ_U64(after.misses, before.misses + 1);
    CHECK_EQ_U64(after.evictions, 3);
    nx_bufpool_destroy(bp);
    plat_file_close(f);
}

static void test_checksum_detects_corruption(void)
{
    char dir[600];
    nx_test_tmpdir("bp_crc", dir, sizeof dir);
    plat_file *f = open_pagefile(dir, "pages.bin");
    REQUIRE(f != NULL);
    nx_bufpool *bp = NULL;
    REQUIRE(nx_bufpool_create(8, &bp) == 0);
    REQUIRE(nx_bufpool_attach(bp, 0, f) == 0);
    fill(bp, 3, 4);
    CHECK(nx_bufpool_flush_simple(bp) == 0);
    nx_bufpool_drop_file(bp, 0);
    uint8_t b = 0;
    uint64_t pos = 3 * NX_PAGE_SIZE + 2000;
    CHECK(plat_pread(f, &b, 1, pos) == 1);
    b ^= 0x40;
    CHECK(plat_pwrite_all(f, &b, 1, pos) == 0);
    nx_frame *fr;
    CHECK(nx_bufpool_pin(bp, 0, 3, NX_PIN_READ, &fr) == NX_BP_CORRUPT);
    nx_bufpool_destroy(bp);
    plat_file_close(f);
}

static void test_page_crc_helpers(void)
{
    uint8_t page[NX_PAGE_SIZE];
    memset(page, 0x11, sizeof page);
    nx_page_set_lsn(page, 42);
    nx_page_seal_crc(page);
    CHECK(nx_page_crc_ok(page));
    CHECK_EQ_U64(nx_page_lsn(page), 42);
    page[4095] ^= 1;
    CHECK(!nx_page_crc_ok(page));
    page[4095] ^= 1;
    page[0] ^= 1; /* the LSN is covered too */
    CHECK(!nx_page_crc_ok(page));
}

static const nx_test_case TESTS[] = {
    NX_CASE(test_write_flush_reread),
    NX_CASE(test_no_steal),
    NX_CASE(test_pinned_frames_stay),
    NX_CASE(test_clock_second_chance),
    NX_CASE(test_checksum_detects_corruption),
    NX_CASE(test_page_crc_helpers),
};

NX_TEST_MAIN(TESTS)
