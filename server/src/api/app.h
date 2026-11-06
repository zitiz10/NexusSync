/*
 * app.h — the running server's state, shared by all handlers: config,
 * engine, sessions and rate limits (auth), the WebSocket hub, the join
 * code, and a background thread for periodic checkpoints.
 *
 * Start-up order (SPEC §8 "while recovering"): the HTTP server starts
 * first with state RECOVERING, so /api/health can answer while
 * nx_app_open_engine replays the log; then state becomes READY.
 */
#ifndef NX_APP_H
#define NX_APP_H

#include "engine.h"
#include "net/ws.h"
#include "util/config.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

enum nx_app_state { NX_APP_RECOVERING = 0, NX_APP_READY = 1, NX_APP_STOPPING = 2, NX_APP_FAILED = 3 };

#define NX_JOIN_CODE_LEN 8
#define NX_PBKDF2_ITERS 600000u

typedef struct nx_auth nx_auth;
typedef struct nx_uploads nx_uploads;
typedef struct nx_stats nx_stats;
typedef struct nx_compare nx_compare;

typedef struct nx_app {
    nx_config cfg;
    nx_engine *engine; /* NULL until READY */
    _Atomic int state;
    nx_hub *hub;
    nx_auth *auth;
    nx_uploads *uploads;
    nx_stats *stats; /* block-map cache, stats.updated throttle */
    nx_compare *compare; /* /api/compare result cache */
    uint32_t pbkdf2_iters; /* SPEC §13: 600,000. Tests lower it in code. */
    uint64_t started_ms;

    pthread_mutex_t join_mu;
    char join_code[NX_JOIN_CODE_LEN + 1];

    pthread_t bg;
    bool bg_running;
    _Atomic bool bg_stop;
} nx_app;

/* Creates the app for an initialized data dir (reads the join code). */
int nx_app_create(const nx_config *cfg, nx_app **out);
/* Opens the engine (runs recovery), emits engine.recovery, starts the
 * background checkpointer, and switches to READY. */
int nx_app_open_engine(nx_app *app);
/* Emits server.shutdown, stops the background thread, closes the engine
 * (final checkpoint) and the hub. */
void nx_app_destroy(nx_app *app);

/* --init helpers: write the first join code for a new data dir. */
int nx_join_code_generate(char out[NX_JOIN_CODE_LEN + 1]);
int nx_join_code_write(const char *data_dir, const char *code);
int nx_join_code_read(const char *data_dir, char out[NX_JOIN_CODE_LEN + 16]);
/* Admin rotation: new random code, persisted atomically. */
int nx_app_rotate_join_code(nx_app *app, char out[NX_JOIN_CODE_LEN + 1]);
void nx_app_join_code(nx_app *app, char out[NX_JOIN_CODE_LEN + 1]);

#endif
