#include "util/log.h"

#include "platform/platform.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>

static enum nx_log_level g_min = NX_LOG_INFO;
/* Serializes whole lines so concurrent workers never interleave output.
 * Leaf lock: taken only here and never while calling anything else. */
static pthread_mutex_t g_log_mu = PTHREAD_MUTEX_INITIALIZER;

static const char *level_name(enum nx_log_level l)
{
    switch (l) {
    case NX_LOG_DEBUG: return "debug";
    case NX_LOG_INFO: return "info";
    case NX_LOG_WARN: return "warn";
    default: return "error";
    }
}

void nx_log_set_level(enum nx_log_level min) { g_min = min; }

void nx_log(enum nx_log_level level, const char *module, const char *fmt, ...)
{
    if (level < g_min)
        return;
    char ts[32];
    plat_format_utc_iso(plat_now_unix_ms(), ts, sizeof ts);
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&g_log_mu);
    fprintf(stderr, "%s %s %s %s\n", ts, level_name(level), module, msg);
    fflush(stderr);
    pthread_mutex_unlock(&g_log_mu);
}
