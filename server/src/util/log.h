/*
 * log.h — one structured line per event on stderr:
 *   2026-11-05T14:27:03.123Z info engine checkpoint pages=12 ms=4
 * Never log passwords, session tokens, join codes, or file contents.
 */
#ifndef NX_LOG_H
#define NX_LOG_H

enum nx_log_level { NX_LOG_DEBUG = 0, NX_LOG_INFO, NX_LOG_WARN, NX_LOG_ERROR };

void nx_log_set_level(enum nx_log_level min);
/* fmt is the message followed by key=value pairs, printf-style. */
void nx_log(enum nx_log_level level, const char *module, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

#define LOG_DEBUG(mod, ...) nx_log(NX_LOG_DEBUG, mod, __VA_ARGS__)
#define LOG_INFO(mod, ...) nx_log(NX_LOG_INFO, mod, __VA_ARGS__)
#define LOG_WARN(mod, ...) nx_log(NX_LOG_WARN, mod, __VA_ARGS__)
#define LOG_ERROR(mod, ...) nx_log(NX_LOG_ERROR, mod, __VA_ARGS__)

#endif
