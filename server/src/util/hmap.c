/* hmap.c — see hmap.h. */
#include "util/hmap.h"

#include <stdlib.h>
#include <string.h>

static size_t round_cap(size_t expected)
{
    size_t cap = 16;
    while (cap * 7 / 10 < expected + 1)
        cap <<= 1;
    return cap;
}

static uint64_t mix64(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    return x ^ (x >> 33);
}

/* ---- 32-byte keys -------------------------------------------------------- */

static uint64_t h32(const uint8_t key[32])
{
    uint64_t x;
    memcpy(&x, key, 8); /* block hashes are already uniformly random */
    return mix64(x);
}

int nx_hmap32_init(nx_hmap32 *m, size_t expected)
{
    memset(m, 0, sizeof *m);
    m->cap = round_cap(expected);
    m->keys = malloc(m->cap * 32);
    m->vals = malloc(m->cap * sizeof *m->vals);
    m->used = calloc(m->cap, 1);
    if (!m->keys || !m->vals || !m->used) {
        nx_hmap32_free(m);
        return -1;
    }
    return 0;
}

void nx_hmap32_free(nx_hmap32 *m)
{
    free(m->keys);
    free(m->vals);
    free(m->used);
    memset(m, 0, sizeof *m);
}

static int grow32(nx_hmap32 *m)
{
    nx_hmap32 n;
    if (nx_hmap32_init(&n, m->count * 2 + 16) != 0)
        return -1;
    for (size_t i = 0; i < m->cap; i++) {
        if (m->used[i] != 1)
            continue;
        bool ins;
        uint64_t *v = nx_hmap32_upsert(&n, m->keys + 32 * i, &ins);
        *v = m->vals[i];
    }
    nx_hmap32_free(m);
    *m = n;
    return 0;
}

uint64_t *nx_hmap32_get(const nx_hmap32 *m, const uint8_t key[32])
{
    size_t mask = m->cap - 1;
    for (size_t i = (size_t)h32(key) & mask, n = 0; n < m->cap; i = (i + 1) & mask, n++) {
        if (m->used[i] == 0)
            return NULL;
        if (m->used[i] == 1 && memcmp(m->keys + 32 * i, key, 32) == 0)
            return &m->vals[i];
    }
    return NULL;
}

uint64_t *nx_hmap32_upsert(nx_hmap32 *m, const uint8_t key[32], bool *inserted)
{
    uint64_t *v = nx_hmap32_get(m, key);
    if (v) {
        if (inserted)
            *inserted = false;
        return v;
    }
    if ((m->count + m->tombstones + 1) * 10 > m->cap * 7 && grow32(m) != 0)
        return NULL;
    size_t mask = m->cap - 1;
    size_t i = (size_t)h32(key) & mask;
    while (m->used[i] == 1)
        i = (i + 1) & mask;
    if (m->used[i] == 2)
        m->tombstones--;
    m->used[i] = 1;
    memcpy(m->keys + 32 * i, key, 32);
    m->vals[i] = 0;
    m->count++;
    if (inserted)
        *inserted = true;
    return &m->vals[i];
}

bool nx_hmap32_remove(nx_hmap32 *m, const uint8_t key[32])
{
    uint64_t *v = nx_hmap32_get(m, key);
    if (!v)
        return false;
    size_t i = (size_t)(v - m->vals);
    m->used[i] = 2;
    m->count--;
    m->tombstones++;
    return true;
}

/* ---- u64 keys ------------------------------------------------------------- */

int nx_hmap64_init(nx_hmap64 *m, size_t expected)
{
    memset(m, 0, sizeof *m);
    m->cap = round_cap(expected);
    m->keys = malloc(m->cap * sizeof *m->keys);
    m->vals = malloc(m->cap * sizeof *m->vals);
    m->used = calloc(m->cap, 1);
    if (!m->keys || !m->vals || !m->used) {
        nx_hmap64_free(m);
        return -1;
    }
    return 0;
}

void nx_hmap64_free(nx_hmap64 *m)
{
    free(m->keys);
    free(m->vals);
    free(m->used);
    memset(m, 0, sizeof *m);
}

static int find64(const nx_hmap64 *m, uint64_t key, size_t *slot)
{
    size_t mask = m->cap - 1;
    for (size_t i = (size_t)mix64(key) & mask, n = 0; n < m->cap; i = (i + 1) & mask, n++) {
        if (m->used[i] == 0)
            return 0;
        if (m->used[i] == 1 && m->keys[i] == key) {
            *slot = i;
            return 1;
        }
    }
    return 0;
}

static int grow64(nx_hmap64 *m)
{
    nx_hmap64 n;
    if (nx_hmap64_init(&n, m->count * 2 + 16) != 0)
        return -1;
    for (size_t i = 0; i < m->cap; i++)
        if (m->used[i] == 1 && nx_hmap64_put(&n, m->keys[i], m->vals[i]) != 0) {
            nx_hmap64_free(&n);
            return -1;
        }
    nx_hmap64_free(m);
    *m = n;
    return 0;
}

int nx_hmap64_put(nx_hmap64 *m, uint64_t key, uint64_t val)
{
    size_t slot;
    if (find64(m, key, &slot)) {
        m->vals[slot] = val;
        return 0;
    }
    if ((m->count + m->tombstones + 1) * 10 > m->cap * 7 && grow64(m) != 0)
        return -1;
    size_t mask = m->cap - 1;
    size_t i = (size_t)mix64(key) & mask;
    while (m->used[i] == 1)
        i = (i + 1) & mask;
    if (m->used[i] == 2)
        m->tombstones--;
    m->used[i] = 1;
    m->keys[i] = key;
    m->vals[i] = val;
    m->count++;
    return 0;
}

bool nx_hmap64_get(const nx_hmap64 *m, uint64_t key, uint64_t *val)
{
    size_t slot;
    if (!find64(m, key, &slot))
        return false;
    if (val)
        *val = m->vals[slot];
    return true;
}

bool nx_hmap64_remove(nx_hmap64 *m, uint64_t key)
{
    size_t slot;
    if (!find64(m, key, &slot))
        return false;
    m->used[slot] = 2;
    m->count--;
    m->tombstones++;
    return true;
}
