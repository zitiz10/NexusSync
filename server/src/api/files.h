/*
 * files.h — the files API (SPEC §12 "Files").
 *
 * Who sees what (SPEC §13): a student sees their own files and files
 * others shared (read only); the admin sees everything. Changing a file
 * (share, delete) needs the owner or the admin. A file flagged deleted
 * (soft delete, D-017) is hidden everywhere.
 *
 * The list shows one row per version chain: its newest version. A file
 * is the newest when no other file names it as prev_version_id.
 */
#ifndef NX_FILES_H
#define NX_FILES_H

#include "net/router.h"

#include <stdbool.h>

/* {id, name, version, prevVersionId, ownerId, ownerName, size, chunkCount,
 *  chunker, sha256, bytesSent, reusedPercent, createdAt, shared}. Leaves
 * the object open so callers can add members, then close it. */
void nx_json_file_open(nx_json *j, nx_engine *e, const nx_file *f);
/* Reused share of the file's bytes that did not cross the network. */
double nx_reused_percent(uint64_t size, uint64_t bytes_sent);

bool nx_file_can_read(const nx_user *u, const nx_file *f);
bool nx_file_can_write(const nx_user *u, const nx_file *f);

void nx_h_files_list(nx_ctx *c);
void nx_h_file_get(nx_ctx *c);
void nx_h_file_content(nx_ctx *c);
void nx_h_file_patch(nx_ctx *c);
void nx_h_file_delete(nx_ctx *c);

#endif
