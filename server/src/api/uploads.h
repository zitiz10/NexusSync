/*
 * uploads.h — upload sessions (SPEC §12 "Uploads").
 *
 *   1. POST /api/uploads            declare name, size, sha256, chunk count
 *   2. POST .../manifest?page=N     up to 8192 (hash, len) entries -> need
 *   3. POST .../chunks              (u32 index, u32 len, data)* -> accepted
 *   4. POST .../commit              -> File, or 409 with missing indices
 *   5. DELETE /api/uploads/{uid}    cancel
 *
 * The server never trusts the client: every length and index is checked
 * against the manifest, every chunk is re-hashed, and the engine re-hashes
 * again and re-reads the whole file at commit (verify_on_commit).
 *
 * Sessions live in memory (a restart loses them; the client starts a new
 * one, and blocks already stored make that cheap). They expire after
 * upload_session_ttl_s idle. The hashes a session declared are protected
 * from cleanup (M12) through nx_uploads_declares.
 *
 * Limits (DECISIONS D-029): files up to 8 GiB; at most 8 open sessions
 * per user and 4,194,304 declared chunks across all sessions (~150 MB of
 * manifests).
 */
#ifndef NX_UPLOADS_H
#define NX_UPLOADS_H

#include "net/router.h"

#include <stdbool.h>
#include <stdint.h>

#define NX_MANIFEST_PAGE 8192u
#define NX_UPLOAD_MAX_SIZE (8ull << 30)
#define NX_UPLOADS_PER_USER 8u
#define NX_UPLOAD_CHUNK_BUDGET 4194304u

typedef struct nx_uploads nx_uploads;

int nx_uploads_create(nx_uploads **out);
void nx_uploads_destroy(nx_uploads *u);
/* Drops sessions idle longer than ttl_s. Returns how many. */
size_t nx_uploads_expire(nx_uploads *u, uint32_t ttl_s);
size_t nx_uploads_count(nx_uploads *u);
/* True if an open session declared this block (cleanup must keep it). */
bool nx_uploads_declares(nx_uploads *u, const uint8_t hash[32]);

void nx_h_upload_create(nx_ctx *c);
void nx_h_upload_manifest(nx_ctx *c);
void nx_h_upload_chunks(nx_ctx *c);
void nx_h_upload_commit(nx_ctx *c);
void nx_h_upload_cancel(nx_ctx *c);

#endif
