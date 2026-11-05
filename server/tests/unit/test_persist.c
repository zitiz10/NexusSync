/*
 * test_persist.c — small on-disk formats of M4:
 *   - recipes: append/read round trip, reopen, corruption and mismatch
 *     detection, a torn tail does not affect earlier records
 *   - superblock: A/B alternation, newest valid copy wins, a damaged
 *     copy falls back to the other, wrong chunker parameters detected
 *   - doublewrite: round trip; truncated or damaged batches are ignored
 *   - WAL record payloads: encode/decode round trips
 */
#include "engine/checkpoint.h"
#include "engine/logrec.h"
#include "engine/recipe.h"
#include "engine/superblock.h"
#include "nx_test.h"
#include "nx_test_util.h"

static void fill_chunks(nx_chunk_ref *c, uint32_t n, uint64_t seed)
{
    nx_splitmix s = {seed};
    for (uint32_t i = 0; i < n; i++) {
        for (int k = 0; k < 4; k++) {
            uint64_t z = nx_splitmix_next(&s);
            memcpy(c[i].hash + 8 * k, &z, 8);
        }
        c[i].len = 2048 + (uint32_t)(nx_splitmix_next(&s) % 14000);
    }
}

static int patch_byte(const char *path, uint64_t off, uint8_t xor_mask)
{
    plat_file *f;
    if (plat_file_open(path, PLAT_O_WRITE, &f) != 0)
        return -1;
    uint8_t b;
    int rc = plat_pread(f, &b, 1, off) == 1 ? 0 : -1;
    b ^= xor_mask;
    if (rc == 0)
        rc = plat_pwrite_all(f, &b, 1, off);
    plat_file_close(f);
    return rc;
}

static void test_recipes(void)
{
    char dir[600], path[700];
    nx_test_tmpdir("recipes", dir, sizeof dir);
    snprintf(path, sizeof path, "%s/recipes.dat", dir);
    nx_recipes *r;
    REQUIRE(nx_recipes_open(path, &r) == 0);
    enum { N = 50 };
    uint64_t off[N];
    uint32_t len[N], cnt[N];
    nx_chunk_ref *c = malloc(3000 * sizeof *c);
    REQUIRE(c);
    for (int i = 0; i < N; i++) {
        cnt[i] = (uint32_t)(i * 61 % 3000); /* includes an empty file (0 chunks) */
        fill_chunks(c, cnt[i], (uint64_t)i);
        REQUIRE(nx_recipes_append(r, 100 + (uint64_t)i, c, cnt[i], &off[i], &len[i]) == 0);
        CHECK_EQ_U64(len[i], nx_recipe_record_len(cnt[i]));
    }
    nx_recipes_close(r);

    REQUIRE(nx_recipes_open(path, &r) == 0);
    for (int i = 0; i < N; i++) {
        nx_chunk_ref *got;
        REQUIRE(nx_recipes_read(r, off[i], len[i], 100 + (uint64_t)i, cnt[i], &got) == 0);
        fill_chunks(c, cnt[i], (uint64_t)i);
        CHECK(cnt[i] == 0 || memcmp(got, c, cnt[i] * sizeof *c) == 0);
        free(got);
    }
    nx_chunk_ref *got;
    /* Wrong file id, wrong count, wrong length: refused. */
    CHECK(nx_recipes_read(r, off[3], len[3], 999, cnt[3], &got) == NX_RECIPE_CORRUPT);
    CHECK(nx_recipes_read(r, off[3], len[3], 103, cnt[3] + 1, &got) == NX_RECIPE_CORRUPT);
    CHECK(nx_recipes_read(r, off[3], len[3] + 36, 103, cnt[3], &got) == NX_RECIPE_CORRUPT);
    CHECK(nx_recipes_read(r, 0, len[3], 103, cnt[3], &got) == NX_RECIPE_CORRUPT);
    nx_recipes_close(r);

    /* A flipped bit inside one record is caught by its CRC; neighbours fine. */
    REQUIRE(patch_byte(path, off[7] + 40, 0x10) == 0);
    REQUIRE(nx_recipes_open(path, &r) == 0);
    CHECK(nx_recipes_read(r, off[7], len[7], 107, cnt[7], &got) == NX_RECIPE_CORRUPT);
    REQUIRE(nx_recipes_read(r, off[8], len[8], 108, cnt[8], &got) == 0);
    free(got);
    nx_recipes_close(r);

    /* Torn tail: half of a record at the end. New appends go after it and
     * every committed record still reads back. */
    plat_file *f;
    REQUIRE(plat_file_open(path, PLAT_O_WRITE, &f) == 0);
    uint64_t size;
    REQUIRE(plat_file_size(f, &size) == 0);
    uint8_t junk[100];
    memset(junk, 0xAB, sizeof junk);
    memcpy(junk, "NXRC", 4);
    REQUIRE(plat_pwrite_all(f, junk, sizeof junk, size) == 0);
    plat_file_close(f);
    REQUIRE(nx_recipes_open(path, &r) == 0);
    uint64_t o2;
    uint32_t l2;
    fill_chunks(c, 10, 4242);
    REQUIRE(nx_recipes_append(r, 500, c, 10, &o2, &l2) == 0);
    CHECK_EQ_U64(o2, size + sizeof junk);
    REQUIRE(nx_recipes_read(r, o2, l2, 500, 10, &got) == 0);
    CHECK(memcmp(got, c, 10 * sizeof *c) == 0);
    free(got);
    REQUIRE(nx_recipes_read(r, off[N - 1], len[N - 1], 100 + N - 1, cnt[N - 1], &got) == 0);
    free(got);
    nx_recipes_close(r);
    free(c);
}

static void test_superblock_ab(void)
{
    char dir[600], pa[700], pb[700];
    nx_test_tmpdir("superblock", dir, sizeof dir);
    snprintf(pa, sizeof pa, "%s/superblock.a", dir);
    snprintf(pb, sizeof pb, "%s/superblock.b", dir);
    nx_superblock sb, got;
    CHECK(nx_superblock_load(dir, &got) == 1); /* not initialized */
    nx_superblock_init(&sb, 1762300000);
    CHECK(nx_superblock_params_match(&sb));
    for (uint64_t i = 1; i <= 5; i++) {
        sb.checkpoint_lsn = i * 100;
        REQUIRE(nx_superblock_write(dir, &sb) == 0);
        CHECK_EQ_U64(sb.seq, i);
        REQUIRE(nx_superblock_load(dir, &got) == 0);
        CHECK_EQ_U64(got.seq, i);
        CHECK_EQ_U64(got.checkpoint_lsn, i * 100);
    }
    /* seq 5 is in .a (odd). Damage it: the loader falls back to seq 4 (.b). */
    REQUIRE(patch_byte(pa, 20, 0x01) == 0);
    REQUIRE(nx_superblock_load(dir, &got) == 0);
    CHECK_EQ_U64(got.seq, 4);
    CHECK_EQ_U64(got.checkpoint_lsn, 400);
    /* The next write (seq 5 again, since we continue from the loaded copy)
     * goes to .a and repairs it. */
    got.checkpoint_lsn = 555;
    REQUIRE(nx_superblock_write(dir, &got) == 0);
    REQUIRE(nx_superblock_load(dir, &sb) == 0);
    CHECK_EQ_U64(sb.seq, 5);
    CHECK_EQ_U64(sb.checkpoint_lsn, 555);
    /* Both damaged: refuse to guess. */
    REQUIRE(patch_byte(pa, 30, 0x01) == 0);
    REQUIRE(patch_byte(pb, 30, 0x01) == 0);
    CHECK(nx_superblock_load(dir, &got) == -1);

    uint8_t page[NX_SUPERBLOCK_SIZE];
    nx_superblock_init(&sb, 1);
    sb.cdc_avg = 8192;
    nx_superblock_encode(&sb, page);
    REQUIRE(nx_superblock_decode(page, &got) == 0);
    CHECK(!nx_superblock_params_match(&got));
}

static void test_doublewrite(void)
{
    char dir[600], path[700];
    nx_test_tmpdir("doublewrite", dir, sizeof dir);
    snprintf(path, sizeof path, "%s/doublewrite.bin", dir);
    nx_dw_page *pages;
    uint8_t *store;
    size_t n;
    uint64_t lsn;
    REQUIRE(nx_dw_load(path, &pages, &store, &n, &lsn) == 0); /* missing file */
    CHECK(n == 0);

    enum { P = 20 };
    uint8_t *data = nx_splitmix_bytes(99, P * 4096);
    nx_dw_page in[P];
    for (uint32_t i = 0; i < P; i++)
        in[i] = (nx_dw_page){i % 4, 1000 + i, data + i * 4096};
    REQUIRE(nx_dw_write(path, in, P, 777) == 0);
    REQUIRE(nx_dw_load(path, &pages, &store, &n, &lsn) == 0);
    CHECK_EQ_U64(n, P);
    CHECK_EQ_U64(lsn, 777);
    for (size_t i = 0; i < n && i < P; i++)
        CHECK(pages[i].file_id == in[i].file_id && pages[i].page_no == in[i].page_no &&
              memcmp(pages[i].data, in[i].data, 4096) == 0);
    free(pages);
    free(store);

    /* A damaged page inside the batch: the whole batch is ignored. */
    REQUIRE(patch_byte(path, NX_DW_HEADER + 5 * NX_DW_ENTRY + 100, 0x40) == 0);
    REQUIRE(nx_dw_load(path, &pages, &store, &n, &lsn) == 0);
    CHECK(n == 0);

    /* A batch cut short (crash while staging): ignored. */
    REQUIRE(nx_dw_write(path, in, P, 778) == 0);
    plat_file *f;
    REQUIRE(plat_file_open(path, PLAT_O_WRITE, &f) == 0);
    REQUIRE(plat_file_truncate(f, NX_DW_HEADER + 7 * NX_DW_ENTRY) == 0);
    plat_file_close(f);
    REQUIRE(nx_dw_load(path, &pages, &store, &n, &lsn) == 0);
    CHECK(n == 0);
    /* Everything except the trailer: ignored too. */
    REQUIRE(nx_dw_write(path, in, P, 779) == 0);
    REQUIRE(plat_file_open(path, PLAT_O_WRITE, &f) == 0);
    REQUIRE(plat_file_truncate(f, NX_DW_HEADER + P * NX_DW_ENTRY) == 0);
    plat_file_close(f);
    REQUIRE(nx_dw_load(path, &pages, &store, &n, &lsn) == 0);
    CHECK(n == 0);

    REQUIRE(nx_dw_clear(path) == 0);
    REQUIRE(nx_dw_load(path, &pages, &store, &n, &lsn) == 0);
    CHECK(n == 0);
    free(data);
}

static void test_log_records(void)
{
    nx_txn t;
    nx_txn_init(&t);
    nx_index_entry e = {.container_id = 3, .offset = 4096, .len = 5000, .created_at = 1762300000};
    memset(e.hash, 0x5A, 32);
    REQUIRE(nx_txn_chunk_add(&t, &e) == 0);
    REQUIRE(nx_txn_ref(&t, NX_REC_REF_INC, e.hash, 7) == 0);
    nx_file f;
    memset(&f, 0, sizeof f);
    f.id = 9;
    snprintf(f.name, sizeof f.name, "x.csv");
    f.size = 12345;
    REQUIRE(nx_txn_file_add(&t, &f) == 0);
    REQUIRE(nx_txn_file_update(&t, 9, 3) == 0);
    REQUIRE(nx_txn_block_move(&t, e.hash, 8, 64) == 0);
    for (int i = 0; i < 200; i++) /* grows the buffers */
        REQUIRE(nx_txn_file_del(&t, (uint64_t)i) == 0);
    REQUIRE(t.n == 205);

    nx_index_entry e2;
    REQUIRE(nx_rec_decode_chunk_add(nx_txn_payload(&t, 0), t.recs[0].len, &e2) == 0);
    CHECK(memcmp(e.hash, e2.hash, 32) == 0 && e2.container_id == 3 && e2.offset == 4096 &&
          e2.len == 5000 && e2.created_at == 1762300000 && e2.refcount == 0);
    uint8_t h[32];
    uint32_t cnt;
    REQUIRE(nx_rec_decode_ref(nx_txn_payload(&t, 1), t.recs[1].len, h, &cnt) == 0);
    CHECK(cnt == 7 && t.recs[1].type == NX_REC_REF_INC);
    nx_file f2;
    REQUIRE(nx_file_decode(nx_txn_payload(&t, 2), t.recs[2].len, &f2) == 0);
    CHECK(f2.id == 9 && f2.size == 12345);
    uint64_t id;
    uint8_t flags;
    REQUIRE(nx_rec_decode_file_update(nx_txn_payload(&t, 3), t.recs[3].len, &id, &flags) == 0);
    CHECK(id == 9 && flags == 3);
    uint32_t cid, off;
    REQUIRE(nx_rec_decode_block_move(nx_txn_payload(&t, 4), t.recs[4].len, h, &cid, &off) == 0);
    CHECK(cid == 8 && off == 64);
    REQUIRE(nx_rec_decode_u64(nx_txn_payload(&t, 204), t.recs[204].len, &id) == 0);
    CHECK(id == 199);
    /* Wrong lengths are malformed. */
    CHECK(nx_rec_decode_chunk_add(nx_txn_payload(&t, 0), 47, &e2) != 0);
    CHECK(nx_rec_decode_u64(nx_txn_payload(&t, 3), 9, &id) != 0);
    CHECK_STR(nx_rec_type_name(NX_REC_FILE_DEL), "FILE_DEL");
    nx_txn_free(&t);
}

static const nx_test_case TESTS[] = {
    NX_CASE(test_recipes),
    NX_CASE(test_superblock_ab),
    NX_CASE(test_doublewrite),
    NX_CASE(test_log_records),
};

NX_TEST_MAIN(TESTS)
