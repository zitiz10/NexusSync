/*
 * test_crypto.c — SHA-256, SHA-1, HMAC-SHA256, PBKDF2, CRC-32, ct_equal.
 * Two independent references:
 *   1. Published vectors (NIST FIPS 180 examples, RFC 4231, RFC 7914).
 *   2. tests/fixtures/crypto_vectors.txt, computed by Python hashlib/hmac/
 *      zlib on SplitMix64 inputs (scripts/make_fixtures.py).
 */
#include "crypto/kdf.h"
#include "crypto/sha1.h"
#include "crypto/sha256.h"
#include "nx_test.h"
#include "nx_test_util.h"
#include "util/crc32.h"

#include <inttypes.h>

static void hex_of(const uint8_t *d, size_t n, char *out) { nx_hex(d, n, out); }

static void test_sha256_published(void)
{
    char hex[65];
    uint8_t d[32];
    nx_sha256("", 0, d);
    hex_of(d, 32, hex);
    CHECK_STR(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    nx_sha256("abc", 3, d);
    hex_of(d, 32, hex);
    CHECK_STR(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const char *m448 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    nx_sha256(m448, strlen(m448), d);
    hex_of(d, 32, hex);
    CHECK_STR(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    /* One million 'a', fed in uneven pieces to exercise buffering. */
    nx_sha256_ctx c;
    nx_sha256_init(&c);
    char a[1000];
    memset(a, 'a', sizeof a);
    size_t left = 1000000, step = 1;
    while (left > 0) {
        size_t n = step < left ? step : left;
        nx_sha256_update(&c, a, n);
        left -= n;
        step = step % 997 + 1;
    }
    nx_sha256_final(&c, d);
    hex_of(d, 32, hex);
    CHECK_STR(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

static void test_sha1_published(void)
{
    char hex[41];
    uint8_t d[20];
    nx_sha1("abc", 3, d);
    hex_of(d, 20, hex);
    CHECK_STR(hex, "a9993e364706816aba3e25717850c26c9cd0d89d");
    nx_sha1("", 0, d);
    hex_of(d, 20, hex);
    CHECK_STR(hex, "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    /* RFC 6455 §1.3 handshake example. */
    const char *k = "dGhlIHNhbXBsZSBub25jZQ==258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    nx_sha1(k, strlen(k), d);
    hex_of(d, 20, hex);
    CHECK_STR(hex, "b37a4f2cc0624f1690f64606cf385945b2bec4ea"); /* base64: s3pPLMBiTxaQ9kYGzzhZRbK+xOo= */
}

static void test_hmac_rfc4231(void)
{
    char hex[65];
    uint8_t d[32], key[131];
    memset(key, 0x0b, 20);
    nx_hmac_sha256(key, 20, "Hi There", 8, d);
    hex_of(d, 32, hex);
    CHECK_STR(hex, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    nx_hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28, d);
    hex_of(d, 32, hex);
    CHECK_STR(hex, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    memset(key, 0xaa, 131);
    const char *m = "Test Using Larger Than Block-Size Key - Hash Key First";
    nx_hmac_sha256(key, 131, m, strlen(m), d);
    hex_of(d, 32, hex);
    CHECK_STR(hex, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}

static void test_pbkdf2_rfc7914(void)
{
    char hex[129];
    uint8_t d[64];
    nx_pbkdf2_sha256("passwd", 6, "salt", 4, 1, d, 64);
    hex_of(d, 64, hex);
    CHECK_STR(hex, "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc"
                   "49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783");
}

static void test_crc32_published(void)
{
    CHECK_EQ_U64(nx_crc32(0, "123456789", 9), 0xCBF43926u);
    CHECK_EQ_U64(nx_crc32(0, "", 0), 0);
    /* Incremental equals one-shot. */
    uint32_t a = nx_crc32(0, "1234", 4);
    CHECK_EQ_U64(nx_crc32(a, "56789", 5), 0xCBF43926u);
}

static void test_ct_equal(void)
{
    uint8_t a[32], b[32];
    memset(a, 7, sizeof a);
    memcpy(b, a, sizeof b);
    CHECK(nx_ct_equal(a, b, 32));
    b[31] ^= 1;
    CHECK(!nx_ct_equal(a, b, 32));
    b[31] ^= 1;
    b[0] ^= 0x80;
    CHECK(!nx_ct_equal(a, b, 32));
    CHECK(nx_ct_equal(a, b, 0));
}

/* Walks crypto_vectors.txt and checks every line. */
static void test_python_reference_vectors(void)
{
    char path[512];
    snprintf(path, sizeof path, "%s/crypto_vectors.txt", nx_fixtures_dir());
    size_t len = 0;
    char *text = (char *)nx_read_file(path, &len);
    REQUIRE(text != NULL);
    int counts[5] = {0};
    char *cursor = text;
    for (char *line = nx_next_line(&cursor); line; line = nx_next_line(&cursor)) {
        if (line[0] == '#' || line[0] == '\0')
            continue;
        char kind[16], want[200], got[200];
        uint64_t s1, n1, s2, n2, iters, dklen;
        if (sscanf(line, "sha256 %" SCNu64 " %" SCNu64 " %199s", &s1, &n1, want) == 3) {
            uint8_t *in = nx_splitmix_bytes(s1, (size_t)n1), d[32];
            nx_sha256(in, (size_t)n1, d);
            /* Also hash it in two pieces split at an awkward point. */
            nx_sha256_ctx c;
            uint8_t d2[32];
            nx_sha256_init(&c);
            nx_sha256_update(&c, in, (size_t)(n1 / 3));
            nx_sha256_update(&c, in + n1 / 3, (size_t)(n1 - n1 / 3));
            nx_sha256_final(&c, d2);
            free(in);
            hex_of(d, 32, got);
            CHECK_STR(got, want);
            CHECK(memcmp(d, d2, 32) == 0);
            counts[0]++;
        } else if (sscanf(line, "sha1 %" SCNu64 " %" SCNu64 " %199s", &s1, &n1, want) == 3) {
            uint8_t *in = nx_splitmix_bytes(s1, (size_t)n1), d[20];
            nx_sha1(in, (size_t)n1, d);
            free(in);
            hex_of(d, 20, got);
            CHECK_STR(got, want);
            counts[1]++;
        } else if (sscanf(line, "crc32 %" SCNu64 " %" SCNu64 " %199s", &s1, &n1, want) == 3) {
            uint8_t *in = nx_splitmix_bytes(s1, (size_t)n1);
            snprintf(got, sizeof got, "%08" PRIx32, nx_crc32(0, in, (size_t)n1));
            free(in);
            CHECK_STR(got, want);
            counts[2]++;
        } else if (sscanf(line, "hmac %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %199s", &s1,
                          &n1, &s2, &n2, want) == 5) {
            uint8_t *k = nx_splitmix_bytes(s1, (size_t)n1), *m = nx_splitmix_bytes(s2, (size_t)n2);
            uint8_t d[32];
            nx_hmac_sha256(k, (size_t)n1, m, (size_t)n2, d);
            free(k);
            free(m);
            hex_of(d, 32, got);
            CHECK_STR(got, want);
            counts[3]++;
        } else if (sscanf(line,
                          "pbkdf2 %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64
                          " %" SCNu64 " %199s",
                          &s1, &n1, &s2, &n2, &iters, &dklen, want) == 7) {
            uint8_t *p = nx_splitmix_bytes(s1, (size_t)n1), *s = nx_splitmix_bytes(s2, (size_t)n2);
            uint8_t d[96];
            REQUIRE(dklen <= sizeof d);
            nx_pbkdf2_sha256(p, (size_t)n1, s, (size_t)n2, (uint32_t)iters, d, (size_t)dklen);
            free(p);
            free(s);
            hex_of(d, (size_t)dklen, got);
            CHECK_STR(got, want);
            counts[4]++;
        } else {
            sscanf(line, "%15s", kind);
            fprintf(stderr, "    unknown vector line: %.60s\n", line);
            CHECK(0);
        }
    }
    free(text);
    fprintf(stderr, "    checked sha256=%d sha1=%d crc32=%d hmac=%d pbkdf2=%d\n", counts[0],
            counts[1], counts[2], counts[3], counts[4]);
    CHECK(counts[0] > 250 && counts[1] > 10 && counts[2] > 5 && counts[3] > 50 && counts[4] > 8);
}

static void test_pbkdf2_production_cost(void)
{
    /* SPEC §13 uses 600,000 iterations. Report how long one login costs. */
    uint8_t d[32];
    uint64_t t0 = plat_now_mono_ms();
    nx_pbkdf2_sha256("correct horse battery", 21, "0123456789abcdef", 16, 600000, d, 32);
    uint64_t ms = plat_now_mono_ms() - t0;
    fprintf(stderr, "    pbkdf2 600k iterations: %" PRIu64 " ms\n", ms);
    CHECK(ms < 20000); /* generous: sanitizer builds are slow */
}

static void test_sha256_throughput(void)
{
    size_t n = 64u << 20;
    uint8_t *buf = malloc(n);
    REQUIRE(buf != NULL);
    memset(buf, 0x5a, n);
    uint8_t d[32];
    uint64_t t0 = plat_now_mono_us();
    nx_sha256(buf, n, d);
    uint64_t us = plat_now_mono_us() - t0;
    free(buf);
    fprintf(stderr, "    sha256 throughput: %.0f MB/s (this build)\n",
            (double)n / (1024.0 * 1024.0) / ((double)us / 1e6));
}

static const nx_test_case TESTS[] = {
    NX_CASE(test_sha256_published),
    NX_CASE(test_sha1_published),
    NX_CASE(test_hmac_rfc4231),
    NX_CASE(test_pbkdf2_rfc7914),
    NX_CASE(test_crc32_published),
    NX_CASE(test_ct_equal),
    NX_CASE(test_python_reference_vectors),
    NX_CASE(test_pbkdf2_production_cost),
    NX_CASE(test_sha256_throughput),
};

NX_TEST_MAIN(TESTS)
