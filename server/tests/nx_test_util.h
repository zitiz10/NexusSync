/*
 * nx_test_util.h — helpers shared by the C tests.
 * nx_splitmix_bytes() must stay identical to splitmix_bytes() in
 * scripts/make_fixtures.py: the crypto vectors depend on it.
 */
#ifndef NX_TEST_UTIL_H
#define NX_TEST_UTIL_H

#include "platform/platform.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t state;
} nx_splitmix;

static inline uint64_t nx_splitmix_next(nx_splitmix *s)
{
    s->state += 0x9E3779B97F4A7C15ull;
    uint64_t z = s->state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/* n bytes of successive outputs, each little-endian. Caller frees. */
static inline uint8_t *nx_splitmix_bytes(uint64_t seed, size_t n)
{
    uint8_t *out = malloc(n ? n : 1);
    if (!out) {
        fprintf(stderr, "out of memory\n");
        exit(2);
    }
    nx_splitmix s = {seed};
    for (size_t i = 0; i < n; i += 8) {
        uint64_t z = nx_splitmix_next(&s);
        for (size_t b = 0; b < 8 && i + b < n; b++)
            out[i + b] = (uint8_t)(z >> (8 * b));
    }
    return out;
}

/* Directory of committed fixtures, relative to server/ (where make runs). */
static inline const char *nx_fixtures_dir(void)
{
    const char *d = getenv("NX_FIXTURES");
    return d ? d : "tests/fixtures";
}

/* Reads a whole file. Returns a malloc'd buffer (NUL-terminated for text
 * use, not counted in *len) or NULL. */
static inline uint8_t *nx_read_file(const char *path, size_t *len)
{
    plat_file *f = NULL;
    if (plat_file_open(path, PLAT_O_READ, &f) != 0)
        return NULL;
    uint64_t size = 0;
    if (plat_file_size(f, &size) != 0 || size > (uint64_t)SIZE_MAX - 1) {
        plat_file_close(f);
        return NULL;
    }
    uint8_t *buf = malloc((size_t)size + 1);
    if (!buf) {
        plat_file_close(f);
        return NULL;
    }
    int64_t got = plat_pread(f, buf, (size_t)size, 0);
    plat_file_close(f);
    if (got < 0 || (uint64_t)got != size) {
        free(buf);
        return NULL;
    }
    buf[size] = '\0';
    *len = (size_t)size;
    return buf;
}

/* Portable line iterator (strtok_r is not in ISO C): returns the next
 * line with its newline replaced by NUL, or NULL at the end. */
static inline char *nx_next_line(char **cursor)
{
    char *start = *cursor;
    if (!start || *start == '\0')
        return NULL;
    char *nl = strchr(start, '\n');
    if (nl) {
        *nl = '\0';
        *cursor = nl + 1;
    } else {
        *cursor = start + strlen(start);
    }
    size_t n = strlen(start);
    if (n > 0 && start[n - 1] == '\r')
        start[n - 1] = '\0';
    return start;
}

#endif
