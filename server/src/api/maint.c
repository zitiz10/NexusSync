/* maint.c — cleanup and integrity-scan jobs. See maint.h and D-048. */
#include "api/maint.h"

#include "api/app.h"
#include "api/uploads.h"
#include "crypto/sha256.h"
#include "util/bytes.h"
#include "util/crc32.h"
#include "util/log.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NIGHTLY_HOUR 2 /* SPEC §10: nightly at 02:00 local time */
#define FILES_PER_FAILURE 20

struct nx_maint {
    char dir[1024];
    /* A leaf lock: held only to read or change the fields below. */
    pthread_mutex_t mu;
    enum nx_job running; /* 0: idle */
    bool thread_live;    /* th exists and has not been joined */
    pthread_t th;
    unsigned scan_percent;
    bool in_scan; /* the scan half of the job is running (nightly: after cleanup) */
    uint64_t next_run_s; /* next nightly run (unix s); 0 until the first tick */
    uint64_t gc_at_ms, gc_reclaimed;
    uint64_t scan_at_ms, scan_verified, scan_failures;
};

typedef struct {
    nx_app *app;
    enum nx_job job;
} job_arg;

/* ===== data/scan_last =========================================================== */

static void scan_path(const char *dir, char *out, size_t n, bool tmp)
{
    snprintf(out, n, "%s/scan_last%s", dir, tmp ? ".tmp" : "");
}

int nx_scan_last_write(const char *data_dir, uint64_t at_ms, uint64_t verified, uint64_t failures)
{
    uint8_t b[NX_SCAN_LAST_LEN] = {'N', 'X', 'S', 'N'};
    nx_put_u16(b + 4, 1);
    nx_put_u64(b + 8, at_ms);
    nx_put_u64(b + 16, verified);
    nx_put_u64(b + 24, failures);
    nx_put_u32(b + 32, nx_crc32(0, b, 32));
    char tmp[1100], path[1100];
    scan_path(data_dir, tmp, sizeof tmp, true);
    scan_path(data_dir, path, sizeof path, false);
    plat_file *f;
    if (plat_file_open(tmp, PLAT_O_WRITE | PLAT_O_CREATE | PLAT_O_TRUNC, &f) != 0)
        return -1;
    int rc = plat_pwrite_all(f, b, sizeof b, 0);
    if (rc == 0)
        rc = plat_fsync_file(f);
    if (plat_file_close(f) != 0)
        rc = -1;
    /* Atomic replace: a crash leaves the old result or the new one. */
    if (rc == 0)
        rc = plat_rename_replace(tmp, path);
    if (rc == 0)
        rc = plat_fsync_dir(data_dir);
    return rc;
}

int nx_scan_last_read(const char *data_dir, uint64_t *at_ms, uint64_t *verified, uint64_t *failures)
{
    char path[1100];
    scan_path(data_dir, path, sizeof path, false);
    plat_file *f;
    if (plat_file_open(path, PLAT_O_READ, &f) != 0)
        return 1;
    uint8_t b[NX_SCAN_LAST_LEN];
    int64_t got = plat_pread(f, b, sizeof b, 0);
    plat_file_close(f);
    if (got != (int64_t)sizeof b || memcmp(b, "NXSN", 4) != 0 || nx_get_u16(b + 4) != 1 ||
        nx_get_u32(b + 32) != nx_crc32(0, b, 32))
        return -1;
    *at_ms = nx_get_u64(b + 8);
    *verified = nx_get_u64(b + 16);
    *failures = nx_get_u64(b + 24);
    return 0;
}

/* ===== lifecycle ================================================================== */

int nx_maint_create(const char *data_dir, nx_maint **out)
{
    nx_maint *m = calloc(1, sizeof *m);
    if (!m)
        return -1;
    snprintf(m->dir, sizeof m->dir, "%s", data_dir);
    pthread_mutex_init(&m->mu, NULL);
    int rc = nx_scan_last_read(data_dir, &m->scan_at_ms, &m->scan_verified, &m->scan_failures);
    if (rc < 0) {
        LOG_WARN("maint", "data/scan_last is damaged; the Engine screen shows no last scan");
        m->scan_at_ms = m->scan_verified = m->scan_failures = 0;
    }
    *out = m;
    return 0;
}

void nx_maint_destroy(nx_maint *m)
{
    if (!m)
        return;
    pthread_mutex_lock(&m->mu);
    bool live = m->thread_live;
    m->thread_live = false;
    pthread_mutex_unlock(&m->mu);
    if (live)
        pthread_join(m->th, NULL); /* a job cannot be stopped half way; let it finish */
    pthread_mutex_destroy(&m->mu);
    free(m);
}


/* ===== cleanup ===================================================================== */

static const char *trigger_of(enum nx_job job) { return job == NX_JOB_NIGHTLY ? "nightly" : "admin"; }

static bool keep_declared(const uint8_t hash[32], void *ctx)
{
    return nx_uploads_declares(ctx, hash);
}

static void gc_progress(const char *phase, uint64_t done, uint64_t total, void *ctx)
{
    nx_app *app = ctx;
    if (strcmp(phase, "compacting") != 0 || total == 0)
        return;
    char data[128];
    snprintf(data, sizeof data, "{\"phase\":\"compacting\",\"done\":%" PRIu64 ",\"total\":%" PRIu64 "}",
             done, total);
    nx_hub_publish_transient(app->hub, "engine.gc", data, (nx_audience){NX_AUD_ADMINS, 0});
}

static void run_gc(nx_app *app, enum nx_job job)
{
    char data[512];
    snprintf(data, sizeof data, "{\"phase\":\"freeing\",\"trigger\":\"%s\"}", trigger_of(job));
    nx_hub_publish_transient(app->hub, "engine.gc", data, (nx_audience){NX_AUD_ADMINS, 0});
    nx_gc_options o = {.grace_s = app->cfg.gc_grace_s,
                       .keep = keep_declared,
                       .keep_ctx = app->uploads,
                       .progress = gc_progress,
                       .progress_ctx = app};
    nx_gc_report r;
    int rc = nx_engine_gc(app->engine, &o, &r);
    if (rc != 0) {
        snprintf(data, sizeof data, "{\"phase\":\"failed\",\"trigger\":\"%s\"}", trigger_of(job));
        nx_hub_publish(app->hub, "engine.gc", data, (nx_audience){NX_AUD_ADMINS, 0});
        return;
    }
    snprintf(data, sizeof data,
             "{\"phase\":\"done\",\"trigger\":\"%s\",\"blocksFreed\":%" PRIu64 ",\"bytesFreed\":%" PRIu64
             ",\"containersCompacted\":%" PRIu64 ",\"containersSkipped\":%" PRIu64
             ",\"blocksMoved\":%" PRIu64 ",\"bytesMoved\":%" PRIu64 ",\"bytesReclaimed\":%" PRIu64
             ",\"durationMs\":%" PRIu64 "}",
             trigger_of(job), r.blocks_freed, r.bytes_freed, r.containers_compacted,
             r.containers_skipped, r.blocks_moved, r.bytes_moved, r.bytes_reclaimed, r.ms);
    pthread_mutex_lock(&app->maint->mu);
    app->maint->gc_at_ms = plat_now_unix_ms();
    app->maint->gc_reclaimed = r.bytes_reclaimed;
    pthread_mutex_unlock(&app->maint->mu);
    nx_hub_publish(app->hub, "engine.gc", data, (nx_audience){NX_AUD_ADMINS, 0});
}

/* ===== integrity scan ================================================================ */

static void scan_progress(const char *phase, uint64_t done, uint64_t total, void *ctx)
{
    (void)phase;
    nx_app *app = ctx;
    unsigned pct = total ? (unsigned)(done * 100u / total) : 100u;
    pthread_mutex_lock(&app->maint->mu);
    app->maint->scan_percent = pct;
    pthread_mutex_unlock(&app->maint->mu);
    char data[128];
    snprintf(data, sizeof data,
             "{\"phase\":\"scanning\",\"done\":%" PRIu64 ",\"total\":%" PRIu64 ",\"percent\":%u}", done,
             total, pct);
    nx_hub_publish_transient(app->hub, "engine.scan", data, (nx_audience){NX_AUD_ADMINS, 0});
}

/* The files that use each damaged block, as JSON arrays. */
typedef struct {
    nx_json *per; /* one array body per failure */
    size_t *count;
} files_ctx;

static int add_file(size_t which, const nx_file *f, void *vctx)
{
    files_ctx *x = vctx;
    if (x->count[which]++ >= FILES_PER_FAILURE)
        return 0;
    nx_json *j = &x->per[which];
    nx_json_obj_open(j);
    nx_json_key_u64(j, "id", f->id);
    nx_json_key_str(j, "name", f->name);
    nx_json_obj_close(j);
    return 0;
}

static const char *reason_name(uint8_t r)
{
    return r == NX_SCAN_CRC ? "crc" : r == NX_SCAN_HASH ? "hash" : "unreadable";
}

static void failure_json(nx_json *j, const nx_scan_failure *f, const nx_json *files)
{
    char hex[65];
    nx_hex(f->hash, 32, hex);
    nx_json_obj_open(j);
    nx_json_key_str(j, "sha256", hex);
    nx_json_key_u64(j, "containerId", f->container_id);
    nx_json_key_u64(j, "offset", f->offset);
    nx_json_key_str(j, "reason", reason_name(f->reason));
    nx_json_key(j, "files");
    nx_json_arr_open(j);
    if (files->len)
        nx_json_raw(j, files->buf, files->len);
    nx_json_arr_close(j);
    nx_json_obj_close(j);
}

static void run_scan(nx_app *app, enum nx_job job)
{
    char small[160];
    pthread_mutex_lock(&app->maint->mu);
    app->maint->in_scan = true;
    app->maint->scan_percent = 0;
    pthread_mutex_unlock(&app->maint->mu);
    nx_scan_report *r = malloc(sizeof *r);
    if (!r || nx_engine_scan(app->engine, scan_progress, app, r) != 0) {
        free(r);
        snprintf(small, sizeof small, "{\"phase\":\"failed\",\"trigger\":\"%s\"}", trigger_of(job));
        nx_hub_publish(app->hub, "engine.scan", small, (nx_audience){NX_AUD_ADMINS, 0});
        return;
    }
    /* Name the files each damaged block belongs to. */
    nx_json per[NX_SCAN_MAX_ITEMS];
    size_t count[NX_SCAN_MAX_ITEMS] = {0};
    uint8_t(*hashes)[32] = r->n_items ? malloc(r->n_items * sizeof *hashes) : NULL;
    for (size_t i = 0; i < r->n_items; i++) {
        nx_json_init(&per[i]);
        if (hashes)
            memcpy(hashes[i], r->items[i].hash, 32);
    }
    files_ctx fx = {per, count};
    if (hashes && nx_engine_files_using(app->engine, (const uint8_t(*)[32])hashes, r->n_items, add_file,
                                        &fx) != 0)
        LOG_WARN("maint", "could not list the files that use the damaged blocks");
    free(hashes);
    /* per[i] holds "{..},{..}" (the writer adds the commas): the body of
     * that failure's files array. */
    nx_json done;
    nx_json_init(&done);
    nx_json_obj_open(&done);
    nx_json_key_str(&done, "phase", "done");
    nx_json_key_str(&done, "trigger", trigger_of(job));
    nx_json_key_u64(&done, "verified", r->verified);
    nx_json_key_u64(&done, "failures", r->failures);
    nx_json_key_u64(&done, "durationMs", r->ms);
    nx_json_key(&done, "items");
    nx_json_arr_open(&done);
    for (size_t i = 0; i < r->n_items; i++) {
        failure_json(&done, &r->items[i], &per[i]);
        nx_json one;
        nx_json_init(&one);
        failure_json(&one, &r->items[i], &per[i]);
        if (!one.oom)
            nx_hub_publish(app->hub, "engine.corruption", one.buf, (nx_audience){NX_AUD_ADMINS, 0});
        nx_json_free(&one);
        nx_json_free(&per[i]);
    }
    nx_json_arr_close(&done);
    nx_json_obj_close(&done);

    uint64_t at = plat_now_unix_ms();
    if (nx_scan_last_write(app->cfg.data_dir, at, r->verified, r->failures) != 0)
        LOG_WARN("maint", "could not save the scan result in data/scan_last");
    pthread_mutex_lock(&app->maint->mu);
    app->maint->scan_at_ms = at;
    app->maint->scan_verified = r->verified;
    app->maint->scan_failures = r->failures;
    app->maint->scan_percent = 100;
    pthread_mutex_unlock(&app->maint->mu);
    if (!done.oom)
        nx_hub_publish(app->hub, "engine.scan", done.buf, (nx_audience){NX_AUD_ADMINS, 0});
    nx_json_free(&done);
    free(r);
}

/* ===== the job thread ===================================================================== */

static void *job_main(void *varg)
{
    job_arg a = *(job_arg *)varg;
    free(varg);
    LOG_INFO("maint", "job started job=%s trigger=%s",
             a.job == NX_JOB_GC ? "cleanup" : a.job == NX_JOB_SCAN ? "scan" : "cleanup+scan",
             trigger_of(a.job));
    if (a.job == NX_JOB_GC || a.job == NX_JOB_NIGHTLY)
        run_gc(a.app, a.job);
    if (a.job == NX_JOB_SCAN || a.job == NX_JOB_NIGHTLY)
        run_scan(a.app, a.job);
    pthread_mutex_lock(&a.app->maint->mu);
    a.app->maint->running = 0;
    a.app->maint->in_scan = false;
    pthread_mutex_unlock(&a.app->maint->mu);
    return NULL;
}

int nx_maint_start(nx_app *app, enum nx_job job)
{
    nx_maint *m = app->maint;
    job_arg *a = malloc(sizeof *a);
    if (!a)
        return -1;
    a->app = app;
    a->job = job;
    pthread_mutex_lock(&m->mu);
    if (m->running) {
        pthread_mutex_unlock(&m->mu);
        free(a);
        return 1;
    }
    /* The previous job has finished (running is 0, set as its last step
     * before returning): reap its thread. */
    if (m->thread_live) {
        pthread_join(m->th, NULL);
        m->thread_live = false;
    }
    m->running = job;
    m->scan_percent = 0;
    if (pthread_create(&m->th, NULL, job_main, a) != 0) {
        m->running = 0;
        pthread_mutex_unlock(&m->mu);
        free(a);
        LOG_ERROR("maint", "could not start a maintenance thread");
        return -1;
    }
    m->thread_live = true;
    pthread_mutex_unlock(&m->mu);
    return 0;
}

void nx_maint_wait(nx_app *app)
{
    for (;;) {
        pthread_mutex_lock(&app->maint->mu);
        bool busy = app->maint->running != 0;
        pthread_mutex_unlock(&app->maint->mu);
        if (!busy)
            return;
        plat_sleep_ms(10);
    }
}

void nx_maint_schedule(nx_app *app, uint64_t at_s)
{
    pthread_mutex_lock(&app->maint->mu);
    app->maint->next_run_s = at_s;
    pthread_mutex_unlock(&app->maint->mu);
}

void nx_maint_tick(nx_app *app)
{
    nx_maint *m = app->maint;
    uint64_t now = plat_now_unix_s();
    pthread_mutex_lock(&m->mu);
    if (m->next_run_s == 0)
        m->next_run_s = plat_next_local_time(now, NIGHTLY_HOUR, 0);
    /* A laptop that slept through 02:00 runs the job once on waking. */
    bool due = now >= m->next_run_s;
    if (due)
        m->next_run_s = plat_next_local_time(now, NIGHTLY_HOUR, 0);
    pthread_mutex_unlock(&m->mu);
    if (due && nx_maint_start(app, NX_JOB_NIGHTLY) == 1)
        LOG_INFO("maint", "nightly cleanup skipped: a job is already running");
}

/* ===== /api/stats members ========================================================================= */

static void key_time(nx_json *j, const char *key, uint64_t unix_ms)
{
    nx_json_key(j, key);
    if (unix_ms == 0) {
        nx_json_null(j);
        return;
    }
    char iso[40];
    plat_format_utc_iso(unix_ms, iso, sizeof iso);
    nx_json_str(j, iso);
}

void nx_maint_json_cleanup(nx_app *app, nx_json *j, uint64_t pending_bytes)
{
    nx_maint *m = app->maint;
    pthread_mutex_lock(&m->mu);
    if (m->next_run_s == 0)
        m->next_run_s = plat_next_local_time(plat_now_unix_s(), NIGHTLY_HOUR, 0);
    uint64_t next = m->next_run_s, at = m->gc_at_ms, rec = m->gc_reclaimed;
    bool running = m->running == NX_JOB_GC || (m->running == NX_JOB_NIGHTLY && !m->in_scan);
    pthread_mutex_unlock(&m->mu);
    nx_json_key(j, "cleanup");
    nx_json_obj_open(j);
    nx_json_key_u64(j, "pendingBytes", pending_bytes);
    key_time(j, "nextRunAt", next * 1000u);
    nx_json_key_bool(j, "running", running);
    key_time(j, "lastRunAt", at);
    nx_json_key_u64(j, "lastReclaimedBytes", rec);
    nx_json_obj_close(j);
}

void nx_maint_json_integrity(nx_app *app, nx_json *j)
{
    nx_maint *m = app->maint;
    pthread_mutex_lock(&m->mu);
    uint64_t at = m->scan_at_ms, ver = m->scan_verified, fail = m->scan_failures;
    /* The nightly job counts as scanning only once its cleanup is done. */
    bool running = m->running != 0 && m->in_scan;
    unsigned pct = m->scan_percent;
    pthread_mutex_unlock(&m->mu);
    nx_json_key(j, "integrity");
    nx_json_obj_open(j);
    key_time(j, "lastScanAt", at);
    nx_json_key_u64(j, "verified", ver);
    nx_json_key_u64(j, "failures", fail);
    nx_json_key_bool(j, "running", running);
    nx_json_key_u64(j, "percent", running ? pct : 0);
    nx_json_obj_close(j);
}

/* ===== handlers ===================================================================================== */

static void start_job(nx_ctx *c, enum nx_job job)
{
    int rc = nx_maint_start(c->app, job);
    if (rc == 1) {
        nx_reply_error(c, 409, "maintenance_running",
                       "Cleanup or an integrity scan is already running. Try again when it is done.");
        return;
    }
    if (rc != 0) {
        nx_reply_error(c, 500, "internal", "The job could not be started.");
        return;
    }
    LOG_INFO("admin", "maintenance requested job=%s by=%" PRIu32, job == NX_JOB_GC ? "cleanup" : "scan",
             c->user.id);
    nx_json j;
    nx_json_init(&j);
    nx_json_obj_open(&j);
    nx_json_key_str(&j, "started", job == NX_JOB_GC ? "gc" : "scan");
    nx_json_obj_close(&j);
    nx_reply_json(c, 202, &j, NULL);
    nx_json_free(&j);
}

void nx_h_admin_gc(nx_ctx *c) { start_job(c, NX_JOB_GC); }

void nx_h_admin_scan(nx_ctx *c) { start_job(c, NX_JOB_SCAN); }
