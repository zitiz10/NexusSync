/*
 * test_chunk.c — fixed and content-defined chunkers (SPEC §6).
 *   - cut bounds hold for every input length
 *   - streaming in any piece size gives exactly the whole-buffer result
 *   - cut points match tests/fixtures/chunks_expected.txt (stability; the
 *     TS chunker is checked against the same file)
 *   - the dedup property: after a 300-byte insertion, CDC re-finds almost
 *     every block while fixed-size blocks stop matching
 */
#include "chunk_fixtures.h"
#include "nx_test.h"
#include "nx_test_util.h"

#include <inttypes.h>

static uint8_t *load_fixture(const char *name, size_t *n)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/data/%s", nx_fixtures_dir(), name);
    uint8_t *buf = nx_read_file(path, n);
    if (!buf)
        fprintf(stderr, "    cannot read %s (run python3 scripts/make_fixtures.py)\n", path);
    return buf;
}

static void test_cut_bounds(void)
{
    uint8_t *buf = nx_splitmix_bytes(42, 3 * NX_CDC_MAX);
    for (size_t n = 0; n <= 3 * NX_CDC_MAX; n += (n < 5000 ? 1 : 97)) {
        size_t c = nx_cdc_cut(buf, n);
        if (n <= NX_CDC_MIN) {
            CHECK_EQ_U64(c, n);
        } else {
            CHECK(c > NX_CDC_MIN);
            CHECK(c <= NX_CDC_MAX);
            CHECK(c <= n);
        }
        size_t f = nx_fixed_cut(buf, n);
        CHECK_EQ_U64(f, n < NX_FIXED_SIZE ? n : NX_FIXED_SIZE);
    }
    /* All-zero input never matches the masks early: Gear(0) is constant,
     * so the cut comes from the hash pattern or from MAX. Either way it is
     * deterministic and within bounds. */
    uint8_t *zeros = calloc(NX_CDC_MAX * 2, 1);
    REQUIRE(zeros != NULL);
    size_t z = nx_cdc_cut(zeros, NX_CDC_MAX * 2);
    CHECK(z > NX_CDC_MIN && z <= NX_CDC_MAX);
    free(zeros);
    free(buf);
}

static void test_streaming_matches_whole_buffer(void)
{
    const char *names[] = {"f05_16385.bin", "f07_10mb_random.bin", "f08_10mb_text.csv"};
    const size_t pieces[] = {1, 7, 4095, 16384, 16385, 65537, 1 << 20};
    for (size_t f = 0; f < 3; f++) {
        size_t n = 0;
        uint8_t *buf = load_fixture(names[f], &n);
        REQUIRE(buf != NULL);
        for (int kind = 0; kind <= 1; kind++) {
            nx_chunk_list whole, part;
            uint8_t dw[32], dp[32];
            REQUIRE(nx_chunk_all((enum nx_chunker_kind)kind, buf, n, 0, &whole, dw) == 0);
            for (size_t p = 0; p < sizeof pieces / sizeof pieces[0]; p++) {
                if (pieces[p] == 1 && n > (1u << 20))
                    continue; /* 1-byte pieces only on the small file */
                REQUIRE(nx_chunk_all((enum nx_chunker_kind)kind, buf, n, pieces[p], &part, dp) == 0);
                CHECK_EQ_U64(part.count, whole.count);
                CHECK(memcmp(dw, dp, 32) == 0);
                nx_chunk_list_free(&part);
            }
            /* Lengths add up to the file. */
            uint64_t total = 0;
            for (size_t k = 0; k < whole.count; k++)
                total += whole.lens[k];
            CHECK_EQ_U64(total, n);
            nx_chunk_list_free(&whole);
        }
        free(buf);
    }
}

static void test_matches_expected_file(void)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/chunks_expected.txt", nx_fixtures_dir());
    size_t len = 0;
    char *text = (char *)nx_read_file(path, &len);
    REQUIRE(text != NULL);
    char *cursor = text;
    int files = 0, cdc = 0, fixed = 0;
    uint8_t *data = NULL;
    size_t n = 0;
    char cur[128] = "";
    for (char *line = nx_next_line(&cursor); line; line = nx_next_line(&cursor)) {
        if (line[0] == '#' || line[0] == '\0')
            continue;
        char kind[8], name[128], hex[65];
        uint64_t a = 0;
        int used = 0;
        if (sscanf(line, "%7s %127s %" SCNu64 " %64s %n", kind, name, &a, hex, &used) < 4) {
            CHECK(0);
            continue;
        }
        if (strcmp(name, cur) != 0) {
            free(data);
            data = load_fixture(name, &n);
            REQUIRE(data != NULL);
            snprintf(cur, sizeof cur, "%s", name);
        }
        uint8_t got[32];
        char ghex[65];
        if (strcmp(kind, "file") == 0) {
            CHECK_EQ_U64(n, a);
            nx_sha256(data, n, got);
            nx_hex(got, 32, ghex);
            CHECK_STR(ghex, hex);
            files++;
            continue;
        }
        enum nx_chunker_kind k = strcmp(kind, "cdc") == 0 ? NX_CHUNKER_CDC : NX_CHUNKER_FIXED;
        nx_chunk_list l;
        REQUIRE(nx_chunk_all(k, data, n, 0, &l, got) == 0);
        nx_hex(got, 32, ghex);
        CHECK_EQ_U64(l.count, a);
        CHECK_STR(ghex, hex);
        if (k == NX_CHUNKER_CDC) {
            /* Compare every length, so a mismatch names the first bad block. */
            const char *p = line + used;
            size_t idx = 0;
            while (*p && *p != '-' && idx < l.count) {
                unsigned long v = strtoul(p, (char **)&p, 10);
                if (v != l.lens[idx]) {
                    fprintf(stderr, "    %s block %zu: expected %lu bytes, got %" PRIu32 "\n", name,
                            idx, v, l.lens[idx]);
                    CHECK(0);
                    break;
                }
                idx++;
                if (*p == ',')
                    p++;
            }
            CHECK_EQ_U64(idx, l.count);
            cdc++;
        } else {
            fixed++;
        }
        nx_chunk_list_free(&l);
    }
    free(data);
    free(text);
    CHECK(files == 10 && cdc == 10 && fixed == 10);
}

static int cmp_hash(const void *a, const void *b) { return memcmp(a, b, 32); }

/* How many blocks of b also appear somewhere in a. */
static size_t shared_blocks(const nx_chunk_list *a, const nx_chunk_list *b)
{
    uint8_t(*sorted)[32] = malloc(a->count * 32 + 1);
    if (!sorted)
        return 0;
    memcpy(sorted, a->hashes, a->count * 32);
    qsort(sorted, a->count, 32, cmp_hash);
    size_t shared = 0;
    for (size_t i = 0; i < b->count; i++)
        if (bsearch(b->hashes[i], sorted, a->count, 32, cmp_hash))
            shared++;
    free(sorted);
    return shared;
}

static void test_insertion_dedup_property(void)
{
    size_t n1 = 0, n2 = 0;
    uint8_t *v1 = load_fixture("f08_10mb_text.csv", &n1);
    uint8_t *v2 = load_fixture("f09_10mb_text_insert.csv", &n2);
    REQUIRE(v1 && v2);
    uint8_t d[32];
    nx_chunk_list c1, c2, f1, f2;
    REQUIRE(nx_chunk_all(NX_CHUNKER_CDC, v1, n1, 0, &c1, d) == 0);
    REQUIRE(nx_chunk_all(NX_CHUNKER_CDC, v2, n2, 0, &c2, d) == 0);
    REQUIRE(nx_chunk_all(NX_CHUNKER_FIXED, v1, n1, 0, &f1, d) == 0);
    REQUIRE(nx_chunk_all(NX_CHUNKER_FIXED, v2, n2, 0, &f2, d) == 0);
    size_t cdc_shared = shared_blocks(&c1, &c2), fixed_shared = shared_blocks(&f1, &f2);
    fprintf(stderr, "    300-byte insert at offset 5000: cdc %zu of %zu blocks reused, fixed %zu of %zu\n",
            cdc_shared, c2.count, fixed_shared, f2.count);
    /* CDC: only the block(s) around the edit change. */
    CHECK(c2.count - cdc_shared <= 3);
    /* Fixed: everything after the edit shifts by 300 bytes; only the block
     * wholly before offset 5000 still matches. */
    CHECK(fixed_shared <= 2);
    nx_chunk_list_free(&c1);
    nx_chunk_list_free(&c2);
    nx_chunk_list_free(&f1);
    nx_chunk_list_free(&f2);
    free(v1);
    free(v2);
}

static void test_cdc_size_distribution(void)
{
    const char *names[] = {"f07_10mb_random.bin", "f08_10mb_text.csv"};
    for (int f = 0; f < 2; f++) {
        size_t n = 0;
        uint8_t *buf = load_fixture(names[f], &n);
        REQUIRE(buf != NULL);
        nx_chunk_list l;
        uint8_t d[32];
        REQUIRE(nx_chunk_all(NX_CHUNKER_CDC, buf, n, 0, &l, d) == 0);
        size_t at_max = 0, small = 0;
        for (size_t k = 0; k + 1 < l.count; k++) { /* last block may be short */
            CHECK(l.lens[k] > NX_CDC_MIN && l.lens[k] <= NX_CDC_MAX);
            at_max += l.lens[k] == NX_CDC_MAX;
            small += l.lens[k] < NX_CDC_AVG;
        }
        double avg = (double)n / (double)l.count;
        fprintf(stderr, "    %s: %zu blocks, average %.0f bytes, %zu below 4 KB, %zu forced at 16 KB\n",
                names[f], l.count, avg, small, at_max);
        /* SPEC §6.2: average somewhat above 4 KB because of MIN. */
        CHECK(avg > 4096.0 && avg < 8192.0);
        nx_chunk_list_free(&l);
        free(buf);
    }
}

static void test_chunk_throughput(void)
{
    size_t n = 0;
    uint8_t *buf = load_fixture("f07_10mb_random.bin", &n);
    REQUIRE(buf != NULL);
    uint64_t t0 = plat_now_mono_us();
    size_t off = 0, blocks = 0;
    while (off < n) {
        off += nx_cdc_cut(buf + off, n - off);
        blocks++;
    }
    uint64_t us = plat_now_mono_us() - t0;
    fprintf(stderr, "    cdc cut-only throughput: %.0f MB/s (this build), %zu blocks\n",
            (double)n / 1048576.0 / ((double)us / 1e6), blocks);
    free(buf);
}

static const nx_test_case TESTS[] = {
    NX_CASE(test_cut_bounds),
    NX_CASE(test_streaming_matches_whole_buffer),
    NX_CASE(test_matches_expected_file),
    NX_CASE(test_insertion_dedup_property),
    NX_CASE(test_cdc_size_distribution),
    NX_CASE(test_chunk_throughput),
};

NX_TEST_MAIN(TESTS)
