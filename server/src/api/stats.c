/* stats.c — storage numbers and the block map. See stats.h. */
#include "api/stats.h"

#include "api/files.h"
#include "api/maint.h"
#include "engine/container.h"
#include "util/hmap.h"
#include "engine/chunk.h"
#include "nexus_version.h"
#include "util/log.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REBUILD_MIN_MS 2000u    /* SPEC §12: recompute at most every 2 s */
#define PUBLISH_MIN_MS 500u     /* stats.updated at most twice a second */
#define WEEK_MS (7ull * 24 * 3600 * 1000)

/* The cached storage layout: every block in storage order, with the byte
 * position where it starts in the physical store. */
typedef struct {
    nx_block_span *spans;
    uint64_t *starts;
    size_t n;
    uint64_t total; /* sum of data bytes = the physical store */
    uint64_t lsn;   /* engine last LSN when built */
    uint64_t built_ms;
} layout;

struct nx_stats {
    pthread_mutex_t mu; /* leaf lock (SPEC §9): never held across an engine call */
    layout *cur;
    bool building;
    uint64_t pub_lsn, pub_ms;
};

int nx_stats_create(nx_stats **out)
{
    nx_stats *s = calloc(1, sizeof *s);
    if (!s)
        return -1;
    pthread_mutex_init(&s->mu, NULL);
    *out = s;
    return 0;
}

static void layout_free(layout *l)
{
    if (!l)
        return;
    free(l->spans);
    free(l->starts);
    free(l);
}

void nx_stats_destroy(nx_stats *s)
{
    if (!s)
        return;
    layout_free(s->cur);
    pthread_mutex_destroy(&s->mu);
    free(s);
}

int nx_share_level(double m)
{
    if (m <= 0.0)
        return 0;
    if (m < 1.5)
        return 1;
    if (m < 2.5)
        return 2;
    if (m < 4.5)
        return 3;
    return 4;
}

/* Tile holding byte position p of a store of `total` bytes cut into n
 * tiles. Tiles are equal slices; a store smaller than n bytes gets one byte
 * per tile so it fills the first tiles instead of scattering. */
static uint32_t tile_of(uint64_t p, uint64_t total, uint32_t n)
{
    if (total <= n)
        return (uint32_t)p;
    /* p < total <= 2^52 in any real lab, n <= 4096: no overflow. */
    return (uint32_t)(p * n / total);
}

/* First byte position of tile k (inverse of tile_of). */
static uint64_t tile_start(uint32_t k, uint64_t total, uint32_t n)
{
    if (total <= n)
        return k;
    return ((uint64_t)k * total + n - 1) / n;
}

size_t nx_stats_tail_tiles(nx_engine *e, uint64_t batch_bytes, uint32_t *out, size_t cap)
{
    nx_engine_stats st;
    nx_engine_get_stats(e, &st);
    uint64_t total = st.physical_bytes;
    if (batch_bytes == 0 || total == 0 || cap == 0)
        return 0;
    if (batch_bytes > total)
        batch_bytes = total;
    uint32_t a = tile_of(total - batch_bytes, total, NX_BLOCKMAP_TILES),
             b = tile_of(total - 1, total, NX_BLOCKMAP_TILES);
    size_t k = 0;
    for (uint32_t t = a; t <= b && k < cap; t++)
        out[k++] = t;
    return k;
}

/* ===== layout cache ============================================================ */

static layout *layout_build(nx_engine *e)
{
    layout *l = calloc(1, sizeof *l);
    if (!l)
        return NULL;
    nx_engine_stats st;
    nx_engine_get_stats(e, &st);
    l->lsn = st.last_lsn; /* taken before the walk: a change during it triggers a rebuild */
    if (nx_engine_block_spans(e, &l->spans, &l->n) != 0) {
        free(l);
        return NULL;
    }
    l->starts = l->n ? malloc(l->n * sizeof *l->starts) : NULL;
    if (l->n && !l->starts) {
        layout_free(l);
        return NULL;
    }
    uint64_t pos = 0;
    for (size_t i = 0; i < l->n; i++) {
        l->starts[i] = pos;
        pos += l->spans[i].len;
    }
    l->total = pos;
    l->built_ms = plat_now_mono_ms();
    return l;
}

/* Returns the current layout with mu held (release with layout_release),
 * rebuilding it first when it is due: the whole map follows the 2 s rule;
 * `force` rebuilds as soon as anything changed, for answers that must not
 * be stale (a file's own tiles right after its upload, the admin's
 * container numbers). Only one rebuild runs at a time; callers arriving
 * during it get the previous layout. */
static layout *layout_get(nx_app *app, bool force)
{
    nx_stats *s = app->stats;
    nx_engine_stats st;
    nx_engine_get_stats(app->engine, &st);
    uint64_t now = plat_now_mono_ms();
    pthread_mutex_lock(&s->mu);
    layout *l = s->cur;
    bool due = !l || (l->lsn != st.last_lsn && (force || now - l->built_ms >= REBUILD_MIN_MS));
    bool build = due && !s->building;
    if (build)
        s->building = true;
    pthread_mutex_unlock(&s->mu);

    if (build) {
        layout *nl = layout_build(app->engine);
        pthread_mutex_lock(&s->mu);
        s->building = false;
        if (nl) {
            /* Readers use a layout only while holding mu, so the old one
             * is not in use now. */
            layout_free(s->cur);
            s->cur = nl;
        } else {
            LOG_WARN("stats", "block map rebuild failed; serving the previous one");
        }
        pthread_mutex_unlock(&s->mu);
    }
    pthread_mutex_lock(&s->mu);
    return s->cur; /* NULL if never built; caller holds mu until layout_release */
}

static void layout_release(nx_app *app)
{
    pthread_mutex_unlock(&app->stats->mu);
}

/* ===== GET /api/stats ============================================================ */

typedef struct {
    uint32_t me;
    uint64_t week_ago_ms;
    uint64_t files, uploads_week;
    uint64_t mine_files, mine_bytes, mine_sent;
} file_tally;

static bool tally_file(const nx_file *f, void *ctx)
{
    file_tally *t = ctx;
    if (f->flags & NX_FILE_DELETED)
        return false;
    t->files++;
    if (f->created_at >= t->week_ago_ms)
        t->uploads_week++;
    if (f->owner_id == t->me) {
        t->mine_files++;
        t->mine_bytes += f->size;
        t->mine_sent += f->bytes_sent < f->size ? f->bytes_sent : f->size;
    }
    return false; /* count only; copy nothing */
}

static void key_iso_or_null(nx_json *j, const char *key, uint64_t unix_ms)
{
    nx_json_key(j, key);
    if (!unix_ms) {
        nx_json_null(j);
        return;
    }
    char at[40];
    plat_format_utc_iso(unix_ms, at, sizeof at);
    nx_json_str(j, at);
}

static double ratio(uint64_t logical, uint64_t physical)
{
    if (physical == 0)
        return logical ? 0.0 : 1.0;
    double r = (double)logical / (double)physical;
    return (double)(int64_t)(r * 100.0 + 0.5) / 100.0; /* two decimals */
}

/* The numbers every signed-in user may see (also the stats.updated data). */
static void global_members(nx_json *j, const nx_engine_stats *st, const file_tally *t)
{
    nx_json_key_u64(j, "logicalBytes", st->logical_bytes);
    nx_json_key_u64(j, "physicalBytes", st->physical_bytes);
    nx_json_key(j, "ratio");
    nx_json_f64(j, ratio(st->logical_bytes, st->physical_bytes));
    nx_json_key_u64(j, "writesSkippedBytes", st->writes_skipped_bytes);
    nx_json_key_u64(j, "networkSavedBytes", st->network_saved_bytes);
    nx_json_key_u64(j, "files", t->files);
    nx_json_key_u64(j, "users", st->users);
    nx_json_key_u64(j, "uploadsThisWeek", t->uploads_week);
}

static int tally(nx_engine *e, uint32_t me, file_tally *t)
{
    memset(t, 0, sizeof *t);
    t->me = me;
    uint64_t now = plat_now_unix_ms();
    t->week_ago_ms = now > WEEK_MS ? now - WEEK_MS : 0;
    nx_file *none;
    size_t n;
    if (nx_catalog_files(nx_engine_catalog(e), tally_file, t, &none, &n) != 0)
        return -1;
    free(none);
    return 0;
}

/* Admin-only: containers, cleanup, from the layout and container sizes. */
static int engine_members(nx_app *app, nx_json *j, const nx_engine_stats *st)
{
    nx_container_info *ci;
    size_t nc;
    if (nx_engine_containers(app->engine, &ci, &nc) != 0)
        return -1;
    uint64_t *live = nc ? calloc(nc, sizeof *live) : NULL;
    if (nc && !live) {
        free(ci);
        return -1;
    }
    uint64_t pending = 0;
    layout *l = layout_get(app, true);
    if (l) {
        /* Both lists are sorted by container id: one merge pass. */
        size_t c = 0;
        for (size_t i = 0; i < l->n; i++) {
            const nx_block_span *sp = &l->spans[i];
            if (sp->refcount == 0) {
                pending += sp->len;
                continue;
            }
            while (c < nc && ci[c].id < sp->container_id)
                c++;
            if (c < nc && ci[c].id == sp->container_id)
                live[c] += NX_RECORD_HEADER + sp->len;
        }
    }
    layout_release(app);

    uint64_t bytes = 0, reclaimable = 0;
    uint64_t hist[10] = {0};
    for (size_t c = 0; c < nc; c++) {
        bytes += ci[c].file_bytes;
        uint64_t data = ci[c].file_bytes > NX_CONTAINER_HEADER
                            ? ci[c].file_bytes - NX_CONTAINER_HEADER : 0;
        if (data == 0)
            continue; /* a fresh, empty container is neither full nor reclaimable */
        uint64_t lv = live[c] < data ? live[c] : data;
        reclaimable += data - lv;
        unsigned b = (unsigned)(lv * 10 / data);
        hist[b > 9 ? 9 : b]++;
    }
    free(live);

    nx_json_key(j, "wal");
    nx_json_obj_open(j);
    nx_json_key_u64(j, "lsn", st->last_lsn);
    nx_json_key_u64(j, "recordsSinceCheckpoint",
                    st->last_lsn > st->checkpoint_lsn ? st->last_lsn - st->checkpoint_lsn : 0);
    nx_json_key_u64(j, "sizeBytes", st->wal_bytes);
    key_iso_or_null(j, "lastCheckpointAt", st->last_checkpoint_at_ms);
    nx_json_key_u64(j, "lastCheckpointMs", st->last_checkpoint_ms);
    nx_json_obj_close(j);

    uint64_t lookups = st->pool.hits + st->pool.misses;
    nx_json_key(j, "index");
    nx_json_obj_open(j);
    nx_json_key_u64(j, "entries", st->index.entries);
    nx_json_key_u64(j, "globalDepth", st->index.global_depth);
    nx_json_key_u64(j, "buckets", st->index.buckets);
    nx_json_key(j, "avgFillPercent");
    nx_json_f64(j, (double)(int64_t)(st->index.avg_fill_percent * 10.0 + 0.5) / 10.0);
    nx_json_key(j, "bufferHitRate");
    if (lookups)
        nx_json_f64(j, (double)(int64_t)(1000.0 * (double)st->pool.hits / (double)lookups + 0.5) / 10.0);
    else
        nx_json_null(j);
    nx_json_key_u64(j, "dirtyPages", st->pool.dirty);
    /* Raw counters, so the Engine screen can chart the hit rate per
     * interval (the cumulative rate barely moves after a while). */
    nx_json_key_u64(j, "bufferHits", st->pool.hits);
    nx_json_key_u64(j, "bufferMisses", st->pool.misses);
    nx_json_obj_close(j);

    nx_json_key(j, "containers");
    nx_json_obj_open(j);
    nx_json_key_u64(j, "count", nc);
    nx_json_key_u64(j, "bytes", bytes);
    nx_json_key_u64(j, "reclaimableBytes", reclaimable);
    nx_json_key(j, "liveHistogram");
    nx_json_arr_open(j);
    for (int b = 0; b < 10; b++)
        nx_json_u64(j, hist[b]);
    nx_json_arr_close(j);
    nx_json_obj_close(j);
    free(ci);

    nx_maint_json_integrity(app, j);

    /* Server facts for the Engine screen (DESIGN §8.6 "Server"). */
    nx_json_key(j, "server");
    nx_json_obj_open(j);
    nx_json_key_str(j, "version", NX_VERSION);
    nx_json_key_str(j, "dataDir", app->cfg.data_dir);
    nx_json_key_str(j, "chunker", app->cfg.chunker == NX_CHUNKER_CDC ? "cdc" : "fixed");
    nx_json_key_u64(j, "cdcMin", NX_CDC_MIN);
    nx_json_key_u64(j, "cdcAvg", NX_CDC_AVG);
    nx_json_key_u64(j, "cdcMax", NX_CDC_MAX);
    nx_json_key_u64(j, "fixedSize", NX_FIXED_SIZE);
    nx_json_key_u64(j, "containerSize", app->cfg.container_size);
    nx_json_key_u64(j, "checkpointIntervalS", app->cfg.checkpoint_interval_s);
    nx_json_key_u64(j, "gcGraceS", app->cfg.gc_grace_s);
    nx_json_obj_close(j);

    nx_maint_json_cleanup(app, j, pending);
    return 0;
}

void nx_h_stats(nx_ctx *c)
{
    nx_app *app = c->app;
    nx_engine_stats st;
    nx_engine_get_stats(app->engine, &st);
    file_tally t;
    if (tally(app->engine, c->user.id, &t) != 0) {
        nx_reply_error(c, 500, "internal", "Could not read the catalog.");
        return;
    }
    nx_json j;
    nx_json_init(&j);
    nx_json_obj_open(&j);
    global_members(&j, &st, &t);
    nx_json_key(&j, "mine");
    nx_json_obj_open(&j);
    nx_json_key_u64(&j, "files", t.mine_files);
    nx_json_key_u64(&j, "bytes", t.mine_bytes);
    nx_json_key(&j, "reusedPercent");
    nx_json_f64(&j, nx_reused_percent(t.mine_bytes, t.mine_sent));
    nx_json_obj_close(&j);
    if (c->user.role == NX_ROLE_ADMIN && engine_members(app, &j, &st) != 0) {
        nx_json_free(&j);
        nx_reply_error(c, 500, "internal", "Could not read the containers.");
        return;
    }
    nx_json_key_u64(&j, "uptimeS", (plat_now_mono_ms() - app->started_ms) / 1000u);
    nx_json_obj_close(&j);
    nx_reply_json(c, 200, &j, "Cache-Control: no-store\r\n");
    nx_json_free(&j);
}

void nx_stats_tick(nx_app *app)
{
    nx_stats *s = app->stats;
    nx_engine_stats st;
    nx_engine_get_stats(app->engine, &st);
    uint64_t now = plat_now_mono_ms();
    pthread_mutex_lock(&s->mu);
    bool due = st.last_lsn != s->pub_lsn && now - s->pub_ms >= PUBLISH_MIN_MS;
    if (due) {
        s->pub_lsn = st.last_lsn;
        s->pub_ms = now;
    }
    pthread_mutex_unlock(&s->mu);
    if (!due)
        return;
    file_tally t;
    if (tally(app->engine, 0, &t) != 0)
        return;
    nx_json j;
    nx_json_init(&j);
    nx_json_obj_open(&j);
    global_members(&j, &st, &t);
    nx_json_obj_close(&j);
    if (!j.oom)
        nx_hub_publish(app->hub, "stats.updated", j.buf, (nx_audience){NX_AUD_ALL, 0});
    nx_json_free(&j);
}

/* ===== GET /api/blockmap ========================================================== */

static int span_find(const layout *l, uint32_t cid, uint32_t off)
{
    size_t lo = 0, hi = l->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const nx_block_span *sp = &l->spans[mid];
        if (sp->container_id < cid || (sp->container_id == cid && sp->offset < off))
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < l->n && l->spans[lo].container_id == cid && l->spans[lo].offset == off)
        return (int)lo; /* n is bounded by the index size, far below INT_MAX */
    return -1;
}

typedef struct {
    uint32_t cid, off;
} where;

/* The distinct stored locations of a file's blocks (index lookups, done
 * without the cache mutex). */
static int file_locations(nx_engine *e, uint64_t file_id, where **out, size_t *count)
{
    nx_file meta;
    nx_chunk_ref *chunks;
    if (nx_engine_file_recipe(e, file_id, &meta, &chunks) != 0)
        return -1;
    nx_hmap32 seen;
    if (nx_hmap32_init(&seen, meta.chunk_count ? meta.chunk_count : 1) != 0) {
        free(chunks);
        return -1;
    }
    where *w = meta.chunk_count ? malloc(meta.chunk_count * sizeof *w) : NULL;
    size_t k = 0;
    int rc = (meta.chunk_count && !w) ? -1 : 0;
    for (uint32_t i = 0; i < meta.chunk_count && rc == 0; i++) {
        bool ins;
        if (!nx_hmap32_upsert(&seen, chunks[i].hash, &ins)) {
            rc = -1;
            break;
        }
        if (!ins)
            continue;
        nx_index_entry ent;
        if (nx_engine_lookup(e, chunks[i].hash, &ent) == 0)
            w[k++] = (where){ent.container_id, ent.offset};
    }
    nx_hmap32_free(&seen);
    free(chunks);
    if (rc != 0) {
        free(w);
        return -1;
    }
    *out = w;
    *count = k;
    return 0;
}

static void blockmap_file(nx_ctx *c, uint64_t file_id, uint32_t ntiles)
{
    nx_app *app = c->app;
    nx_file f;
    if (!nx_catalog_file_by_id(nx_engine_catalog(app->engine), file_id, &f) ||
        !nx_file_can_read(&c->user, &f)) {
        nx_reply_error(c, 404, "not_found", "No such file.");
        return;
    }
    where *w;
    size_t nw;
    if (file_locations(app->engine, file_id, &w, &nw) != 0) {
        nx_reply_error(c, 500, "internal", "Could not read the file's blocks.");
        return;
    }
    uint8_t *mark = calloc(ntiles, 1);
    if (!mark) {
        free(w);
        nx_reply_error(c, 500, "internal", "Out of memory.");
        return;
    }
    uint64_t tile_bytes = 0;
    for (int pass = 0; pass < 2; pass++) {
        size_t missing = 0;
        layout *l = layout_get(app, pass == 1);
        if (l) {
            tile_bytes = l->total ? (l->total + ntiles - 1) / ntiles : 0;
            memset(mark, 0, ntiles);
            for (size_t i = 0; i < nw; i++) {
                int at = span_find(l, w[i].cid, w[i].off);
                if (at < 0) {
                    missing++;
                    continue;
                }
                uint64_t s = l->starts[at], len = l->spans[at].len;
                uint32_t a = tile_of(s, l->total, ntiles), b = tile_of(s + len - 1, l->total, ntiles);
                for (uint32_t t = a; t <= b && t < ntiles; t++)
                    mark[t] = 1;
            }
        } else {
            missing = nw;
        }
        layout_release(app);
        /* Blocks stored after the cached walk (a file uploaded a moment
         * ago): rebuild once, without waiting for the 2 s rule. */
        if (missing == 0 || pass == 1)
            break;
    }
    free(w);
    nx_json j;
    nx_json_init(&j);
    nx_json_obj_open(&j);
    nx_json_key_u64(&j, "fileId", file_id);
    nx_json_key_u64(&j, "tileBytes", tile_bytes);
    nx_json_key(&j, "tiles");
    nx_json_arr_open(&j);
    for (uint32_t t = 0; t < ntiles; t++)
        if (mark[t])
            nx_json_u64(&j, t);
    nx_json_arr_close(&j);
    nx_json_obj_close(&j);
    free(mark);
    nx_reply_json(c, 200, &j, "Cache-Control: no-store\r\n");
    nx_json_free(&j);
}

void nx_h_blockmap(nx_ctx *c)
{
    char v[32];
    uint32_t ntiles = NX_BLOCKMAP_TILES;
    if (nx_query_get(c->req->query, "tiles", v, sizeof v)) {
        char *end;
        unsigned long n = strtoul(v, &end, 10);
        if (*v < '0' || *v > '9' || *end || n < 1 || n > NX_BLOCKMAP_MAX_TILES) {
            nx_reply_error(c, 400, "bad_tiles", "tiles must be a whole number from 1 to 4096.");
            return;
        }
        ntiles = (uint32_t)n;
    }
    if (nx_query_get(c->req->query, "file", v, sizeof v)) {
        char *end;
        unsigned long long id = strtoull(v, &end, 10);
        if (*v < '1' || *v > '9' || *end || id == 0 || id == UINT64_MAX) {
            nx_reply_error(c, 404, "not_found", "No such file.");
            return;
        }
        blockmap_file(c, id, ntiles);
        return;
    }

    /* Whole map: byte-weighted mean share count per tile (D-040). */
    double *refs = calloc(ntiles, sizeof *refs), *bytes = calloc(ntiles, sizeof *bytes);
    if (!refs || !bytes) {
        free(refs);
        free(bytes);
        nx_reply_error(c, 500, "internal", "Out of memory.");
        return;
    }
    uint64_t total = 0, blocks = 0;
    layout *l = layout_get(c->app, false);
    if (l) {
        total = l->total;
        blocks = l->n;
        for (size_t i = 0; i < l->n; i++) {
            const nx_block_span *sp = &l->spans[i];
            if (sp->len == 0)
                continue;
            /* A block waiting for cleanup (refcount 0) still occupies disk
             * and was written once. */
            double r = sp->refcount ? (double)sp->refcount : 1.0;
            uint64_t s = l->starts[i], e = s + sp->len;
            uint32_t a = tile_of(s, total, ntiles), b = tile_of(e - 1, total, ntiles);
            for (uint32_t t = a; t <= b && t < ntiles; t++) {
                uint64_t ts = tile_start(t, total, ntiles), te = tile_start(t + 1, total, ntiles);
                uint64_t lo = s > ts ? s : ts, hi = e < te ? e : te;
                if (hi > lo) {
                    refs[t] += r * (double)(hi - lo);
                    bytes[t] += (double)(hi - lo);
                }
            }
        }
    }
    layout_release(c->app);
    nx_json j;
    nx_json_init(&j);
    nx_json_obj_open(&j);
    nx_json_key_u64(&j, "tileBytes", total ? (total + ntiles - 1) / ntiles : 0);
    nx_json_key_u64(&j, "physicalBytes", total);
    nx_json_key_u64(&j, "blocks", blocks);
    nx_json_key(&j, "tiles");
    nx_json_arr_open(&j);
    for (uint32_t t = 0; t < ntiles; t++)
        nx_json_u64(&j, bytes[t] > 0 ? (uint64_t)nx_share_level(refs[t] / bytes[t]) : 0);
    nx_json_arr_close(&j);
    nx_json_obj_close(&j);
    free(refs);
    free(bytes);
    nx_reply_json(c, 200, &j, "Cache-Control: no-store\r\n");
    nx_json_free(&j);
}
