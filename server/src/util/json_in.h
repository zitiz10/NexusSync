/*
 * json_in.h — parsing request JSON with the vendored cJSON, safely from
 * many threads.
 *
 * cJSON keeps one global "last error" position and writes it on every
 * parse, so two workers parsing at once race on it (ThreadSanitizer found
 * this in M10, DECISIONS D-044). We never read that error, but a data race
 * is undefined behaviour, so every server-side parse goes through here and
 * takes a short leaf mutex (SPEC §9). Request bodies are capped at 1 MB,
 * so the wait is tiny.
 */
#ifndef NX_JSON_IN_H
#define NX_JSON_IN_H

#include "cJSON.h"

#include <stddef.h>

/* NULL on malformed input or out of memory. Free with cJSON_Delete. */
cJSON *nx_json_parse(const void *buf, size_t len);

#endif
