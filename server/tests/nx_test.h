/*
 * nx_test.h — minimal test runner (docs/DECISIONS.md D-007).
 *
 * Usage in a test file:
 *   static void test_something(void) { CHECK(1 + 1 == 2); }
 *   static const nx_test_case TESTS[] = { NX_CASE(test_something) };
 *   NX_TEST_MAIN(TESTS)
 *
 * A failing CHECK prints file:line and the expression, marks the test as
 * failed, and continues, so one run shows every failure. REQUIRE stops
 * the current test (use it when later lines would crash).
 */
#ifndef NX_TEST_H
#define NX_TEST_H

#include "platform/platform.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *name;
    void (*fn)(void);
} nx_test_case;

#define NX_CASE(fn) {#fn, fn}

static int nx_test_failed_checks;
static int nx_test_current_failed;
static char nx_test_tmp_root[512];

#define CHECK(cond)                                                                          \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            fprintf(stderr, "    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
            nx_test_failed_checks++;                                                         \
            nx_test_current_failed = 1;                                                      \
        }                                                                                    \
    } while (0)

#define REQUIRE(cond)                                                                        \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            fprintf(stderr, "    FAIL %s:%d: %s (stopping this test)\n", __FILE__, __LINE__, \
                    #cond);                                                                  \
            nx_test_failed_checks++;                                                         \
            nx_test_current_failed = 1;                                                      \
            return;                                                                          \
        }                                                                                    \
    } while (0)

#define CHECK_EQ_U64(a, b)                                                                   \
    do {                                                                                     \
        uint64_t nx_a_ = (uint64_t)(a), nx_b_ = (uint64_t)(b);                               \
        if (nx_a_ != nx_b_) {                                                                \
            fprintf(stderr, "    FAIL %s:%d: %s == %s (%" PRIu64 " != %" PRIu64 ")\n",       \
                    __FILE__, __LINE__, #a, #b, nx_a_, nx_b_);                               \
            nx_test_failed_checks++;                                                         \
            nx_test_current_failed = 1;                                                      \
        }                                                                                    \
    } while (0)

#define CHECK_STR(a, b)                                                                      \
    do {                                                                                     \
        const char *nx_a_ = (a), *nx_b_ = (b);                                               \
        if (!nx_a_ || !nx_b_ || strcmp(nx_a_, nx_b_) != 0) {                                 \
            fprintf(stderr, "    FAIL %s:%d: %s == %s (\"%s\" != \"%s\")\n", __FILE__,       \
                    __LINE__, #a, #b, nx_a_ ? nx_a_ : "(null)", nx_b_ ? nx_b_ : "(null)");   \
            nx_test_failed_checks++;                                                         \
            nx_test_current_failed = 1;                                                      \
        }                                                                                    \
    } while (0)

/* Creates a fresh, empty scratch directory for the current test and
 * writes its path into buf. Lives under the build dir; removed on pass. */
static inline void nx_test_tmpdir(const char *name, char *buf, size_t n)
{
    snprintf(buf, n, "%s/%s", nx_test_tmp_root, name);
    if (plat_remove_tree(buf) != 0 || plat_mkdir_p(buf) != 0) {
        fprintf(stderr, "cannot create tmp dir %s: %s\n", buf, plat_last_error());
        exit(2);
    }
}

static inline int nx_test_run(const nx_test_case *cases, size_t count, int argc, char **argv)
{
    const char *base = getenv("NX_TEST_TMP");
    /* Name the scratch dir after the test binary: build/tmp/test_wal-1234 */
    const char *prog = argc > 0 && argv[0] ? argv[0] : "test";
    for (const char *p = prog; *p; p++)
        if (*p == '/' || *p == '\\')
            prog = p + 1;
    snprintf(nx_test_tmp_root, sizeof nx_test_tmp_root, "%s/%s-%" PRIu32,
             base ? base : "build/tmp", prog, plat_getpid());
    if (plat_mkdir_p(nx_test_tmp_root) != 0) {
        fprintf(stderr, "cannot create %s: %s\n", nx_test_tmp_root, plat_last_error());
        return 2;
    }
    /* Optional filter: run only tests whose name contains argv[1]. */
    const char *filter = argc > 1 ? argv[1] : NULL;
    int failed_tests = 0, ran = 0;
    for (size_t i = 0; i < count; i++) {
        if (filter && !strstr(cases[i].name, filter))
            continue;
        nx_test_current_failed = 0;
        uint64_t t0 = plat_now_mono_ms();
        cases[i].fn();
        uint64_t ms = plat_now_mono_ms() - t0;
        ran++;
        if (nx_test_current_failed) {
            failed_tests++;
            fprintf(stderr, "  not ok %s (%" PRIu64 " ms)\n", cases[i].name, ms);
        } else {
            fprintf(stderr, "  ok     %s (%" PRIu64 " ms)\n", cases[i].name, ms);
        }
    }
    fprintf(stderr, "  %d/%d passed\n", ran - failed_tests, ran);
    if (failed_tests == 0)
        plat_remove_tree(nx_test_tmp_root); /* keep files of failed runs for debugging */
    return failed_tests == 0 ? 0 : 1;
}

#define NX_TEST_MAIN(cases)                                                                  \
    int main(int argc, char **argv)                                                          \
    {                                                                                        \
        return nx_test_run(cases, sizeof(cases) / sizeof((cases)[0]), argc, argv);           \
    }

#endif
