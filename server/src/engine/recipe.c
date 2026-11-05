/* recipe.c — append-only recipe records. See recipe.h. */
#include "engine/recipe.h"

#include "platform/platform.h"
#include "util/bytes.h"
#include "util/crc32.h"
#include "util/log.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define REC_FIXED (4u + 8u + 4u + 4u) /* magic, file id, count, crc */

struct nx_recipes {
    plat_file *f;
    pthread_mutex_t mu; /* leaf: protects end and the append itself */
    uint64_t end;
};

uint32_t nx_recipe_record_len(uint32_t count)
{
    uint64_t n = (uint64_t)REC_FIXED + (uint64_t)count * NX_RECIPE_CHUNK_BYTES;
    return n > UINT32_MAX ? 0 : (uint32_t)n;
}

static void write_header(uint8_t h[NX_RECIPE_FILE_HEADER])
{
    memset(h, 0, NX_RECIPE_FILE_HEADER);
    memcpy(h, "NXRF", 4);
    nx_put_u16(h + 4, NX_RECIPE_VERSION);
}

/* Directory part of path, for fsyncing the new directory entry. */
static int fsync_parent(const char *path)
{
    const char *slash = strrchr(path, '/');
    if (!slash)
        return plat_fsync_dir(".");
    size_t n = (size_t)(slash - path);
    char *dir = malloc(n + 1);
    if (!dir)
        return -1;
    memcpy(dir, path, n);
    dir[n] = '\0';
    int rc = plat_fsync_dir(dir);
    free(dir);
    return rc;
}

int nx_recipes_open(const char *path, nx_recipes **out)
{
    nx_recipes *r = calloc(1, sizeof *r);
    if (!r)
        return -1;
    bool existed = plat_path_exists(path);
    if (plat_file_open(path, PLAT_O_WRITE | PLAT_O_CREATE, &r->f) != 0) {
        LOG_ERROR("recipe", "cannot open path=%s err=%s", path, plat_last_error());
        free(r);
        return -1;
    }
    uint64_t size = 0;
    if (plat_file_size(r->f, &size) != 0)
        goto fail;
    if (size < NX_RECIPE_FILE_HEADER) {
        /* New file, or one whose header never became durable: no committed
         * record can exist yet (a record fsync would have made the header
         * durable too), so (re)write the header. */
        uint8_t h[NX_RECIPE_FILE_HEADER];
        write_header(h);
        if (plat_file_truncate(r->f, 0) != 0 || plat_pwrite_all(r->f, h, sizeof h, 0) != 0 ||
            plat_fsync_file(r->f) != 0)
            goto fail;
        if (!existed && fsync_parent(path) != 0)
            goto fail;
        size = NX_RECIPE_FILE_HEADER;
    } else {
        uint8_t h[NX_RECIPE_FILE_HEADER];
        if (plat_pread(r->f, h, sizeof h, 0) != (int64_t)sizeof h)
            goto fail;
        if (memcmp(h, "NXRF", 4) != 0 || nx_get_u16(h + 4) != NX_RECIPE_VERSION) {
            LOG_ERROR("recipe", "bad recipe file header path=%s", path);
            goto fail;
        }
    }
    /* A torn record at the tail stays where it is: it is unreachable, and
     * new records simply go after it. */
    r->end = size;
    pthread_mutex_init(&r->mu, NULL);
    *out = r;
    return 0;
fail:
    plat_file_close(r->f);
    free(r);
    return -1;
}

void nx_recipes_close(nx_recipes *r)
{
    if (!r)
        return;
    if (plat_file_close(r->f) != 0)
        LOG_ERROR("recipe", "close failed err=%s", plat_last_error());
    pthread_mutex_destroy(&r->mu);
    free(r);
}

uint64_t nx_recipes_size(nx_recipes *r)
{
    pthread_mutex_lock(&r->mu);
    uint64_t e = r->end;
    pthread_mutex_unlock(&r->mu);
    return e;
}

int nx_recipes_append(nx_recipes *r, uint64_t file_id, const nx_chunk_ref *chunks, uint32_t count,
                      uint64_t *offset, uint32_t *len)
{
    uint32_t n = nx_recipe_record_len(count);
    if (n == 0)
        return -1;
    uint8_t *buf = malloc(n);
    if (!buf)
        return -1;
    memcpy(buf, "NXRC", 4);
    nx_put_u64(buf + 4, file_id);
    nx_put_u32(buf + 12, count);
    uint8_t *p = buf + 16;
    for (uint32_t i = 0; i < count; i++, p += NX_RECIPE_CHUNK_BYTES) {
        memcpy(p, chunks[i].hash, 32);
        nx_put_u32(p + 32, chunks[i].len);
    }
    nx_put_u32(p, nx_crc32(0, buf, n - 4));

    pthread_mutex_lock(&r->mu);
    uint64_t off = r->end;
    int rc = plat_pwrite_all(r->f, buf, n, off);
    if (rc == 0)
        rc = plat_fsync_file(r->f);
    if (rc == 0) {
        r->end = off + n;
    } else {
        /* Leave end where it was: the next append overwrites the bytes of
         * this failed one, which nothing points at. */
        LOG_ERROR("recipe", "append failed file_id=%" PRIu64 " err=%s", file_id,
                  plat_last_error());
    }
    pthread_mutex_unlock(&r->mu);
    free(buf);
    if (rc != 0)
        return -1;
    *offset = off;
    *len = n;
    return 0;
}

int nx_recipes_read(nx_recipes *r, uint64_t offset, uint32_t len, uint64_t file_id,
                    uint32_t expect_count, nx_chunk_ref **chunks)
{
    *chunks = NULL;
    /* The catalog record is trusted only as far as it is self-consistent:
     * the length must be exactly what the chunk count implies. */
    if (len != nx_recipe_record_len(expect_count) || len == 0 || offset < NX_RECIPE_FILE_HEADER)
        return NX_RECIPE_CORRUPT;
    uint8_t *buf = malloc(len);
    nx_chunk_ref *out = malloc((expect_count ? expect_count : 1) * sizeof *out);
    if (!buf || !out) {
        free(buf);
        free(out);
        return -1;
    }
    int64_t got = plat_pread(r->f, buf, len, offset);
    int rc = 0;
    if (got < 0) {
        rc = -1;
    } else if ((uint64_t)got != len || memcmp(buf, "NXRC", 4) != 0 ||
               nx_get_u64(buf + 4) != file_id || nx_get_u32(buf + 12) != expect_count ||
               nx_get_u32(buf + len - 4) != nx_crc32(0, buf, len - 4)) {
        rc = NX_RECIPE_CORRUPT;
    } else {
        const uint8_t *p = buf + 16;
        for (uint32_t i = 0; i < expect_count; i++, p += NX_RECIPE_CHUNK_BYTES) {
            memcpy(out[i].hash, p, 32);
            out[i].len = nx_get_u32(p + 32);
        }
    }
    free(buf);
    if (rc != 0) {
        if (rc == NX_RECIPE_CORRUPT)
            LOG_ERROR("recipe", "corrupt recipe file_id=%" PRIu64 " offset=%" PRIu64, file_id,
                      offset);
        free(out);
        return rc;
    }
    *chunks = out;
    return 0;
}
