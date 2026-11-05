/*
 * container.c — append-only block containers. See container.h.
 *
 * Locks (SPEC §9):
 *   append_mu  = "active container mutex" (#6). Held while appending,
 *                sealing, and fsyncing the active container.
 *   cache_mu   = read-handle cache. A leaf lock: held only to look up or
 *                insert a handle, never while taking any other lock.
 */
#include "engine/container.h"

#include "platform/platform.h"
#include "util/bytes.h"
#include "util/crc32.h"
#include "util/log.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HANDLE_CACHE 64
#define MAX_RECORD_DATA 65536u

struct handle {
    uint32_t id;
    plat_file *f;
    int refs;
    bool doomed; /* dropped while in use: close on last release */
    uint64_t last_use;
};

struct nx_containers {
    char *dir;
    uint64_t size_limit;

    pthread_mutex_t append_mu;
    uint32_t active_id;
    plat_file *active;
    uint64_t append_off;
    bool unsynced;
    uint8_t *scratch; /* record header + data, under append_mu */

    pthread_mutex_t cache_mu;
    struct handle cache[HANDLE_CACHE];
    uint64_t tick;
};

static void container_path(const nx_containers *c, uint32_t id, char *buf, size_t n)
{
    snprintf(buf, n, "%s/c%08" PRIu32 ".dat", c->dir, id);
}

/* ---- creating and validating container files --------------------------- */

static int write_header(plat_file *f, uint32_t id)
{
    uint8_t h[NX_CONTAINER_HEADER];
    memset(h, 0, sizeof h);
    memcpy(h, "NXSC", 4);
    nx_put_u16(h + 4, NX_CONTAINER_VERSION);
    nx_put_u32(h + 8, id);
    nx_put_u64(h + 12, plat_now_unix_s());
    return plat_pwrite_all(f, h, sizeof h, 0);
}

static bool header_valid(plat_file *f, uint32_t id)
{
    uint8_t h[NX_CONTAINER_HEADER];
    if (plat_pread(f, h, sizeof h, 0) != (int64_t)sizeof h)
        return false;
    return memcmp(h, "NXSC", 4) == 0 && nx_get_u16(h + 4) == NX_CONTAINER_VERSION &&
           nx_get_u32(h + 8) == id;
}

/* Creates container `id` with a durable header and opens it for appends. */
static int create_container(nx_containers *c, uint32_t id)
{
    char path[1024];
    container_path(c, id, path, sizeof path);
    plat_file *f = NULL;
    if (plat_file_open(path, PLAT_O_WRITE | PLAT_O_CREATE | PLAT_O_TRUNC, &f) != 0) {
        LOG_ERROR("container", "create failed path=%s err=\"%s\"", path, plat_last_error());
        return -1;
    }
    /* Header and directory entry durable before any record is appended,
     * so a container referenced by the WAL always has a valid header. */
    if (write_header(f, id) != 0 || plat_fsync_file(f) != 0 || plat_fsync_dir(c->dir) != 0) {
        LOG_ERROR("container", "init failed path=%s err=\"%s\"", path, plat_last_error());
        plat_file_close(f);
        return -1;
    }
    c->active = f;
    c->active_id = id;
    c->append_off = NX_CONTAINER_HEADER;
    c->unsynced = false;
    return 0;
}

static int seal_active(nx_containers *c)
{
    if (!c->active)
        return 0;
    int rc = plat_fsync_file(c->active);
    if (rc != 0)
        LOG_ERROR("container", "fsync on seal failed id=%" PRIu32 " err=\"%s\"", c->active_id,
                  plat_last_error());
    if (plat_file_close(c->active) != 0)
        rc = -1;
    c->active = NULL;
    c->unsynced = false;
    return rc;
}

struct list_ctx {
    uint32_t *ids;
    size_t count, cap;
    int err;
};

static int list_cb(const char *name, bool is_dir, void *vctx)
{
    struct list_ctx *l = vctx;
    if (is_dir || strlen(name) != 13 || name[0] != 'c' || strcmp(name + 9, ".dat") != 0)
        return 0;
    uint32_t id = 0;
    for (int i = 1; i <= 8; i++) {
        if (name[i] < '0' || name[i] > '9')
            return 0;
        id = id * 10 + (uint32_t)(name[i] - '0');
    }
    if (id == 0)
        return 0;
    if (l->count == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 64;
        uint32_t *ids = realloc(l->ids, cap * sizeof *ids);
        if (!ids) {
            l->err = 1;
            return 1;
        }
        l->ids = ids;
        l->cap = cap;
    }
    l->ids[l->count++] = id;
    return 0;
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

int nx_containers_list(nx_containers *c, uint32_t **ids, size_t *count)
{
    struct list_ctx l = {NULL, 0, 0, 0};
    if (plat_list_dir(c->dir, list_cb, &l) < 0 || l.err) {
        free(l.ids);
        return -1;
    }
    if (l.count > 1)
        qsort(l.ids, l.count, sizeof *l.ids, cmp_u32);
    *ids = l.ids;
    *count = l.count;
    return 0;
}

/* Returns the offset just past the last complete, CRC-valid record. */
static int valid_end(nx_containers *c, uint32_t id, uint64_t *end, uint64_t *trailing)
{
    /* The scanner without a callback reports where valid records end; it
     * stops at a torn last record. */
    *end = NX_CONTAINER_HEADER;
    return nx_containers_scan(c, id, false, NULL, end, trailing);
}

int nx_containers_open(const char *dir, uint64_t container_size, nx_containers **out)
{
    if (container_size < NX_CONTAINER_HEADER + NX_RECORD_HEADER + 16384 ||
        container_size > UINT32_MAX) {
        LOG_ERROR("container", "bad container_size=%" PRIu64, container_size);
        return -1;
    }
    nx_containers *c = calloc(1, sizeof *c);
    if (!c)
        return -1;
    c->dir = malloc(strlen(dir) + 1);
    c->scratch = malloc(NX_RECORD_HEADER + MAX_RECORD_DATA);
    if (!c->dir || !c->scratch) {
        free(c->dir);
        free(c->scratch);
        free(c);
        return -1;
    }
    strcpy(c->dir, dir);
    c->size_limit = container_size;
    pthread_mutex_init(&c->append_mu, NULL);
    pthread_mutex_init(&c->cache_mu, NULL);

    if (plat_mkdir_p(dir) != 0)
        goto fail;
    uint32_t *ids = NULL;
    size_t n = 0;
    if (nx_containers_list(c, &ids, &n) != 0)
        goto fail;
    uint32_t max_id = n ? ids[n - 1] : 0;
    free(ids);

    if (max_id == 0) {
        if (create_container(c, 1) != 0)
            goto fail;
    } else {
        char path[1024];
        container_path(c, max_id, path, sizeof path);
        plat_file *f = NULL;
        if (plat_file_open(path, PLAT_O_WRITE, &f) != 0)
            goto fail;
        if (!header_valid(f, max_id)) {
            /* Created but its header never became durable, so no committed
             * record can live in it: start it over. */
            plat_file_close(f);
            LOG_WARN("container", "rewriting incomplete container id=%" PRIu32, max_id);
            if (create_container(c, max_id) != 0)
                goto fail;
        } else {
            uint64_t size = 0, end = 0, trailing = 0;
            if (plat_file_size(f, &size) != 0) {
                plat_file_close(f);
                goto fail;
            }
            c->active = f;
            c->active_id = max_id;
            if (valid_end(c, max_id, &end, &trailing) != 0)
                goto fail;
            if (end == size) {
                c->append_off = size;
            } else {
                LOG_WARN("container",
                         "damaged tail sealed id=%" PRIu32 " valid_end=%" PRIu64
                         " orphan_bytes=%" PRIu64,
                         max_id, end, size - end);
                if (seal_active(c) != 0 || create_container(c, max_id + 1) != 0)
                    goto fail;
            }
        }
    }
    *out = c;
    return 0;
fail:
    nx_containers_close(c);
    return -1;
}

void nx_containers_close(nx_containers *c)
{
    if (!c)
        return;
    if (c->active) {
        if (c->unsynced)
            plat_fsync_file(c->active);
        plat_file_close(c->active);
    }
    for (int i = 0; i < HANDLE_CACHE; i++)
        if (c->cache[i].f)
            plat_file_close(c->cache[i].f);
    pthread_mutex_destroy(&c->append_mu);
    pthread_mutex_destroy(&c->cache_mu);
    free(c->scratch);
    free(c->dir);
    free(c);
}

/* ---- appends ----------------------------------------------------------------- */

int nx_containers_append(nx_containers *c, const uint8_t hash[32], const uint8_t *data,
                         uint32_t len, nx_block_loc *loc)
{
    if (len > MAX_RECORD_DATA)
        return -1;
    uint64_t rec = NX_RECORD_HEADER + (uint64_t)len;
    pthread_mutex_lock(&c->append_mu);
    if (c->append_off + rec > c->size_limit && c->append_off > NX_CONTAINER_HEADER) {
        uint32_t next = c->active_id + 1;
        if (seal_active(c) != 0 || create_container(c, next) != 0) {
            pthread_mutex_unlock(&c->append_mu);
            return -1;
        }
    }
    uint8_t *r = c->scratch;
    memcpy(r, hash, 32);
    nx_put_u32(r + 32, len);
    nx_put_u32(r + 36, nx_crc32(0, data, len));
    memcpy(r + NX_RECORD_HEADER, data, len);
    /* One write for header and data keeps a torn append confined to the
     * tail; the CRC tells a complete record from a partial one. */
    if (plat_pwrite_all(c->active, r, (size_t)rec, c->append_off) != 0) {
        LOG_ERROR("container", "append failed id=%" PRIu32 " err=\"%s\"", c->active_id,
                  plat_last_error());
        pthread_mutex_unlock(&c->append_mu);
        return -1;
    }
    loc->container_id = c->active_id;
    loc->offset = (uint32_t)c->append_off;
    loc->len = len;
    c->append_off += rec;
    c->unsynced = true;
    pthread_mutex_unlock(&c->append_mu);
    return 0;
}

int nx_containers_sync(nx_containers *c)
{
    pthread_mutex_lock(&c->append_mu);
    int rc = 0;
    if (c->unsynced) {
        rc = plat_fsync_file(c->active);
        if (rc == 0)
            c->unsynced = false;
        else
            LOG_ERROR("container", "fsync failed id=%" PRIu32 " err=\"%s\"", c->active_id,
                      plat_last_error());
    }
    pthread_mutex_unlock(&c->append_mu);
    return rc;
}

uint32_t nx_containers_active_id(nx_containers *c)
{
    pthread_mutex_lock(&c->append_mu);
    uint32_t id = c->active_id;
    pthread_mutex_unlock(&c->append_mu);
    return id;
}

int nx_containers_rotate(nx_containers *c, uint32_t *new_id)
{
    pthread_mutex_lock(&c->append_mu);
    uint32_t next = c->active_id + 1;
    int rc = seal_active(c);
    if (rc == 0)
        rc = create_container(c, next);
    if (rc == 0 && new_id)
        *new_id = c->active_id;
    pthread_mutex_unlock(&c->append_mu);
    return rc;
}

/* ---- read handles ---------------------------------------------------------------- */

static struct handle *acquire(nx_containers *c, uint32_t id)
{
    pthread_mutex_lock(&c->cache_mu);
    c->tick++;
    for (int i = 0; i < HANDLE_CACHE; i++) {
        struct handle *h = &c->cache[i];
        if (h->f && h->id == id && !h->doomed) {
            h->refs++;
            h->last_use = c->tick;
            pthread_mutex_unlock(&c->cache_mu);
            return h;
        }
    }
    /* Miss: pick an empty slot, else the least recently used idle one. */
    struct handle *slot = NULL;
    for (int i = 0; i < HANDLE_CACHE; i++) {
        struct handle *h = &c->cache[i];
        if (!h->f) {
            slot = h;
            break;
        }
        if (h->refs == 0 && (!slot || h->last_use < slot->last_use))
            slot = h;
    }
    if (!slot) {
        pthread_mutex_unlock(&c->cache_mu);
        LOG_ERROR("container", "read handle cache exhausted");
        return NULL;
    }
    if (slot->f) {
        plat_file_close(slot->f);
        slot->f = NULL;
    }
    char path[1024];
    container_path(c, id, path, sizeof path);
    plat_file *f = NULL;
    /* Opening under cache_mu is a short metadata operation; it keeps two
     * threads from opening the same container twice. */
    if (plat_file_open(path, PLAT_O_READ, &f) != 0) {
        pthread_mutex_unlock(&c->cache_mu);
        return NULL;
    }
    slot->f = f;
    slot->id = id;
    slot->refs = 1;
    slot->doomed = false;
    slot->last_use = c->tick;
    pthread_mutex_unlock(&c->cache_mu);
    return slot;
}

static void release(nx_containers *c, struct handle *h)
{
    pthread_mutex_lock(&c->cache_mu);
    h->refs--;
    if (h->refs == 0 && h->doomed) {
        plat_file_close(h->f);
        h->f = NULL;
        h->doomed = false;
    }
    pthread_mutex_unlock(&c->cache_mu);
}

int nx_containers_read(nx_containers *c, const nx_block_loc *loc, uint8_t *buf, size_t cap,
                       uint8_t hash_out[32], bool *corrupt)
{
    if (corrupt)
        *corrupt = false;
    if (loc->len > cap || loc->len > MAX_RECORD_DATA || loc->offset < NX_CONTAINER_HEADER)
        return -1;
    struct handle *h = acquire(c, loc->container_id);
    if (!h)
        return -1;
    uint8_t hdr[NX_RECORD_HEADER];
    int rc = -1;
    if (plat_pread(h->f, hdr, sizeof hdr, loc->offset) == (int64_t)sizeof hdr &&
        nx_get_u32(hdr + 32) == loc->len &&
        plat_pread(h->f, buf, loc->len, (uint64_t)loc->offset + NX_RECORD_HEADER) ==
            (int64_t)loc->len) {
        if (nx_crc32(0, buf, loc->len) == nx_get_u32(hdr + 36)) {
            if (hash_out)
                memcpy(hash_out, hdr, 32);
            rc = 0;
        }
    }
    if (rc != 0 && corrupt)
        *corrupt = true;
    release(c, h);
    return rc;
}

/* ---- scanning ------------------------------------------------------------------- */

int nx_containers_scan(nx_containers *c, uint32_t id, bool with_data, nx_record_cb cb, void *ctx,
                       uint64_t *trailing_bytes)
{
    struct handle *h = acquire(c, id);
    if (!h)
        return -1;
    uint64_t size = 0;
    if (plat_file_size(h->f, &size) != 0 || size > ((uint64_t)1 << 32)) {
        release(c, h);
        return -1;
    }
    uint8_t *buf = malloc(size ? (size_t)size : 1);
    if (!buf) {
        release(c, h);
        return -1;
    }
    int64_t got = plat_pread(h->f, buf, (size_t)size, 0);
    release(c, h);
    if (got != (int64_t)size) {
        free(buf);
        return -1;
    }
    uint64_t off = NX_CONTAINER_HEADER;
    int rc = 0;
    while (off + NX_RECORD_HEADER <= size) {
        uint32_t len = nx_get_u32(buf + off + 32);
        if (len > MAX_RECORD_DATA || off + NX_RECORD_HEADER + len > size)
            break; /* torn or garbage: cannot trust anything after this */
        const uint8_t *data = buf + off + NX_RECORD_HEADER;
        bool crc_ok = nx_crc32(0, data, len) == nx_get_u32(buf + off + 36);
        if (!crc_ok && off + NX_RECORD_HEADER + len == size)
            break; /* last record damaged: an interrupted append */
        if (cb) {
            nx_block_loc loc = {id, (uint32_t)off, len};
            rc = cb(&loc, buf + off, crc_ok, with_data ? data : NULL, ctx);
            if (rc != 0)
                break;
        }
        off += NX_RECORD_HEADER + len;
    }
    /* Without a callback, ctx receives the end offset of valid records
     * (used by open to find where appends may continue). */
    if (!cb && ctx)
        *(uint64_t *)ctx = off;
    if (trailing_bytes)
        *trailing_bytes = size > off ? size - off : 0;
    free(buf);
    return rc;
}

int nx_containers_file_size(nx_containers *c, uint32_t id, uint64_t *size)
{
    char path[1024];
    container_path(c, id, path, sizeof path);
    plat_file *f = NULL;
    if (plat_file_open(path, PLAT_O_READ, &f) != 0)
        return -1;
    int rc = plat_file_size(f, size);
    plat_file_close(f);
    return rc;
}

int nx_containers_drop(nx_containers *c, uint32_t id)
{
    pthread_mutex_lock(&c->append_mu);
    bool is_active = id == c->active_id;
    pthread_mutex_unlock(&c->append_mu);
    if (is_active)
        return -1; /* never delete the container receiving appends */
    pthread_mutex_lock(&c->cache_mu);
    for (int i = 0; i < HANDLE_CACHE; i++) {
        struct handle *h = &c->cache[i];
        if (h->f && h->id == id) {
            if (h->refs == 0) {
                plat_file_close(h->f);
                h->f = NULL;
            } else {
                h->doomed = true;
            }
        }
    }
    pthread_mutex_unlock(&c->cache_mu);
    char path[1024];
    container_path(c, id, path, sizeof path);
    if (plat_remove(path) != 0)
        return -1;
    return plat_fsync_dir(c->dir);
}
