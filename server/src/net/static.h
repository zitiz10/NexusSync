/*
 * static.h — serves the built web app from web_root (SPEC §11).
 *
 * Path safety: the decoded path must start with '/', contain no NUL,
 * backslash, or ':' (Windows drive letters and alternate data streams),
 * and no segment may be "..", ".", empty, or start with '.' (hidden files
 * such as .env). Anything else is refused before the file system sees it.
 * Unknown paths that do not look like files get index.html, so the
 * browser-side router can handle /files, /engine, and so on.
 */
#ifndef NX_STATIC_H
#define NX_STATIC_H

#include "net/router.h"

#include <stdbool.h>
#include <stddef.h>

/* 0 when path is safe to map under web_root. */
int nx_static_path_ok(const char *decoded_path);
const char *nx_static_mime(const char *path);
void nx_static_serve(nx_ctx *c);

#endif
