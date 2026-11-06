/* app.c — server state, join code, background checkpoints. See app.h. */
#include "api/app.h"

#include "api/admin.h"
#include "api/auth.h"
#include "api/compare.h"
#include "api/stats.h"
#include "api/uploads.h"
#include "util/json_out.h"
#include "util/log.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* No 0/O, 1/I/L: easy to read aloud and type from a projector. */
static const char JOIN_ALPHABET[] = "23456789ABCDEFGHJKMNPQRSTUVWXYZ";

int nx_join_code_generate(char out[NX_JOIN_CODE_LEN + 1])
{
    size_t n = sizeof JOIN_ALPHABET - 1; /* 31 */
    for (size_t i = 0; i < NX_JOIN_CODE_LEN;) {
        uint8_t b;
        if (plat_random_bytes(&b, 1) != 0)
            return -1;
        if (b >= 248) /* 248 = 8 * 31: reject to keep every letter equally likely */
            continue;
        out[i++] = JOIN_ALPHABET[b % n];
    }
    out[NX_JOIN_CODE_LEN] = '\0';
    return 0;
}

static void join_path(const char *data_dir, char *buf, size_t n, bool tmp)
{
    snprintf(buf, n, "%s/join_code%s", data_dir, tmp ? ".tmp" : "");
}

int nx_join_code_write(const char *data_dir, const char *code)
{
    char tmp[1100], path[1100];
    join_path(data_dir, tmp, sizeof tmp, true);
    join_path(data_dir, path, sizeof path, false);
    char line[32];
    int n = snprintf(line, sizeof line, "%s\n", code);
    plat_file *f;
    if (plat_file_open(tmp, PLAT_O_WRITE | PLAT_O_CREATE | PLAT_O_TRUNC, &f) != 0)
        return -1;
    int rc = plat_pwrite_all(f, line, (size_t)n, 0);
    if (rc == 0)
        rc = plat_fsync_file(f);
    if (plat_file_close(f) != 0)
        rc = -1;
    /* Atomic replace: readers see the old code or the new one, never half. */
    if (rc == 0)
        rc = plat_rename_replace(tmp, path);
    if (rc == 0)
        rc = plat_fsync_dir(data_dir);
    return rc;
}

int nx_join_code_read(const char *data_dir, char out[NX_JOIN_CODE_LEN + 16])
{
    char path[1100];
    join_path(data_dir, path, sizeof path, false);
    plat_file *f;
    if (plat_file_open(path, PLAT_O_READ, &f) != 0)
        return -1;
    char buf[NX_JOIN_CODE_LEN + 16];
    int64_t got = plat_pread(f, buf, sizeof buf - 1, 0);
    plat_file_close(f);
    if (got <= 0)
        return -1;
    buf[got] = '\0';
    size_t len = strcspn(buf, "\r\n");
    buf[len] = '\0';
    if (len < 6)
        return -1;
    memcpy(out, buf, len + 1);
    return 0;
}

void nx_app_join_code(nx_app *app, char out[NX_JOIN_CODE_LEN + 1])
{
    pthread_mutex_lock(&app->join_mu);
    memcpy(out, app->join_code, NX_JOIN_CODE_LEN + 1);
    pthread_mutex_unlock(&app->join_mu);
}

int nx_app_rotate_join_code(nx_app *app, char out[NX_JOIN_CODE_LEN + 1])
{
    char code[NX_JOIN_CODE_LEN + 1];
    if (nx_join_code_generate(code) != 0)
        return -1;
    pthread_mutex_lock(&app->join_mu);
    int rc = nx_join_code_write(app->cfg.data_dir, code);
    if (rc == 0)
        memcpy(app->join_code, code, sizeof code);
    pthread_mutex_unlock(&app->join_mu);
    if (rc == 0)
        memcpy(out, code, sizeof code);
    return rc;
}

int nx_app_create(const nx_config *cfg, nx_app **out)
{
    nx_app *app = calloc(1, sizeof *app);
    if (!app)
        return -1;
    app->cfg = *cfg;
    app->pbkdf2_iters = NX_PBKDF2_ITERS;
    app->started_ms = plat_now_mono_ms();
    atomic_init(&app->state, NX_APP_RECOVERING);
    atomic_init(&app->bg_stop, false);
    atomic_init(&app->replayed, 0);
    pthread_mutex_init(&app->join_mu, NULL);
    char code[NX_JOIN_CODE_LEN + 16];
    if (nx_join_code_read(cfg->data_dir, code) != 0) {
        LOG_ERROR("app", "no join code in data dir (run --init) dir=%s", cfg->data_dir);
        goto fail;
    }
    /* Codes are stored upper-case; an 8-character code from the config
     * file (--init) is accepted as typed. */
    for (size_t i = 0; code[i]; i++)
        if (code[i] >= 'a' && code[i] <= 'z')
            code[i] = (char)(code[i] - 32);
    snprintf(app->join_code, sizeof app->join_code, "%.8s", code);
    if (nx_auth_create(cfg->session_ttl_s, cfg->data_dir, &app->auth) != 0 ||
        nx_uploads_create(&app->uploads) != 0 || nx_stats_create(&app->stats) != 0 ||
        nx_compare_create(&app->compare) != 0 ||
        nx_hub_create(&app->hub) != 0)
        goto fail;
    *out = app;
    return 0;
fail:
    nx_compare_destroy(app->compare);
    nx_stats_destroy(app->stats);
    nx_uploads_destroy(app->uploads);
    nx_auth_destroy(app->auth);
    pthread_mutex_destroy(&app->join_mu);
    free(app);
    return -1;
}

static void emit_checkpoint(nx_app *app, const nx_engine_stats *st)
{
    char data[160];
    snprintf(data, sizeof data,
             "{\"lsn\":%" PRIu64 ",\"pages\":%" PRIu64 ",\"durationMs\":%" PRIu64 "}",
             st->checkpoint_lsn, st->last_checkpoint_pages, st->last_checkpoint_ms);
    nx_hub_publish(app->hub, "engine.checkpoint", data, (nx_audience){NX_AUD_ADMINS, 0});
}

/* Runs a checkpoint every checkpoint_interval_s (SPEC §3), and announces
 * every checkpoint the engine ran, whoever triggered it. */
static void *bg_main(void *arg)
{
    nx_app *app = arg;
    nx_engine_stats st;
    nx_engine_get_stats(app->engine, &st);
    uint64_t seen = st.checkpoints;
    uint64_t next = plat_now_mono_ms() + (uint64_t)app->cfg.checkpoint_interval_s * 1000u;
    while (!atomic_load(&app->bg_stop)) {
        plat_sleep_ms(250);
        if (plat_now_mono_ms() >= next) {
            if (nx_engine_checkpoint(app->engine) != 0)
                LOG_ERROR("app", "background checkpoint failed");
            next = plat_now_mono_ms() + (uint64_t)app->cfg.checkpoint_interval_s * 1000u;
        }
        size_t expired = nx_uploads_expire(app->uploads, app->cfg.upload_session_ttl_s);
        if (expired)
            LOG_INFO("app", "upload sessions expired count=%zu", expired);
        nx_engine_get_stats(app->engine, &st);
        if (st.checkpoints != seen) {
            seen = st.checkpoints;
            if (st.last_checkpoint_pages > 0) /* skip no-op checkpoints */
                emit_checkpoint(app, &st);
        }
        nx_stats_tick(app);
    }
    return NULL;
}

int nx_app_open_engine(nx_app *app)
{
    nx_engine_config ec;
    nx_engine_config_defaults(&ec);
    ec.data_dir = app->cfg.data_dir;
    ec.container_size = app->cfg.container_size;
    ec.pool_pages = app->cfg.buffer_pool_pages;
    ec.verify_on_read = app->cfg.verify_on_read;
    ec.verify_on_commit = app->cfg.verify_on_commit;
    ec.replay_progress = &app->replayed;
    if (nx_engine_open(&ec, &app->engine) != 0) {
        atomic_store(&app->state, NX_APP_FAILED);
        return -1;
    }
    nx_engine_recovery ri;
    nx_engine_last_recovery(app->engine, &ri);
    char data[256];
    snprintf(data, sizeof data,
             "{\"recordsReplayed\":%" PRIu64 ",\"transactionsRedone\":%" PRIu64
             ",\"pagesRepaired\":%" PRIu64 ",\"durationMs\":%" PRIu64 "}",
             ri.records_replayed, ri.txns_redone, ri.pages_repaired, ri.ms);
    nx_hub_publish(app->hub, "engine.recovery", data, (nx_audience){NX_AUD_ADMINS, 0});
    /* Recovery history for the Engine screen (D-046). Losing it is not a
     * reason to refuse to start. */
    if (nx_recovery_log_append(app->cfg.data_dir, plat_now_unix_ms(), &ri) != 0)
        LOG_WARN("app", "could not record this start in data/recoveries");
    if (pthread_create(&app->bg, NULL, bg_main, app) == 0)
        app->bg_running = true;
    else
        LOG_WARN("app", "no background checkpoint thread; checkpoints run on demand only");
    atomic_store(&app->state, NX_APP_READY);
    return 0;
}

void nx_app_destroy(nx_app *app)
{
    if (!app)
        return;
    atomic_store(&app->state, NX_APP_STOPPING);
    if (app->hub)
        nx_hub_publish(app->hub, "server.shutdown", NULL, (nx_audience){NX_AUD_ALL, 0});
    atomic_store(&app->bg_stop, true);
    if (app->bg_running)
        pthread_join(app->bg, NULL);
    nx_hub_destroy(app->hub); /* delivers server.shutdown first */
    if (app->engine && nx_engine_close(app->engine) != 0)
        LOG_ERROR("app", "final checkpoint failed; the next start recovers from the log");
    nx_uploads_destroy(app->uploads);
    nx_stats_destroy(app->stats);
    nx_compare_destroy(app->compare);
    nx_auth_destroy(app->auth);
    pthread_mutex_destroy(&app->join_mu);
    free(app);
}
