/*
 * catalog.h — users and files tables (SPEC §5, §7.4).
 *
 * On disk: two files of 4096-byte slotted pages, read and written through
 * the buffer pool so they share the checkpoint and recovery path with the
 * block index. Slotted page layout:
 *   header 16 bytes: u64 page_lsn, u16 slot_count, u16 free_offset, u32 crc
 *   slot array right after the header: u16 offset, u16 length (0 = free)
 *   records packed from the end of the page towards the slot array
 *
 * In memory: every record is also kept decoded, with indexes by id (users,
 * files), by username, and by owner + name (versions). They are rebuilt by
 * scanning the pages at startup.
 *
 * Changes arrive as WAL records and are applied here. Applying is
 * idempotent by key ("logical redo"): adding a file that already exists or
 * deleting one that is gone does nothing, so recovery may replay a change
 * that already reached the pages.
 *
 * Locking: the catalog rwlock (SPEC §9 #2) is taken inside each function.
 */
#ifndef NX_CATALOG_H
#define NX_CATALOG_H

#include "engine/bufpool.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NX_USERNAME_MAX 32
#define NX_DISPLAY_MAX 64
#define NX_FILENAME_MAX 255

enum { NX_ROLE_STUDENT = 0, NX_ROLE_ADMIN = 1 };
enum { NX_STATUS_ACTIVE = 0, NX_STATUS_DISABLED = 1 };
enum { NX_FILE_SHARED = 1u, NX_FILE_DELETED = 2u };

typedef struct {
    uint32_t id;
    uint8_t role, status;
    char username[NX_USERNAME_MAX + 1];
    char display[NX_DISPLAY_MAX + 1];
    uint8_t salt[16];
    uint8_t pw_hash[32];
    uint32_t pbkdf2_iters;
    uint64_t created_at, last_seen;
} nx_user;

typedef struct {
    uint64_t id;
    uint32_t owner_id;
    char name[NX_FILENAME_MAX + 1];
    uint32_t version;
    uint64_t prev_version_id;
    uint64_t size;
    uint32_t chunk_count;
    uint8_t chunker;
    uint64_t recipe_offset;
    uint32_t recipe_len;
    uint8_t sha256[32];
    uint64_t bytes_sent;
    uint64_t created_at;
    uint8_t flags;
} nx_file;

/* Serialized forms (also used as WAL payloads). Return bytes written or
 * needed; decode returns 0 or -1 on malformed input. */
#define NX_USER_REC_MAX (4 + 1 + 1 + 2 + NX_USERNAME_MAX + 2 + NX_DISPLAY_MAX + 16 + 32 + 4 + 8 + 8)
#define NX_FILE_REC_MAX (8 + 4 + 2 + NX_FILENAME_MAX + 4 + 8 + 8 + 4 + 1 + 8 + 4 + 32 + 8 + 8 + 1)
size_t nx_user_encode(const nx_user *u, uint8_t *out);
int nx_user_decode(const uint8_t *in, size_t len, nx_user *u);
size_t nx_file_encode(const nx_file *f, uint8_t *out);
int nx_file_decode(const uint8_t *in, size_t len, nx_file *f);

typedef struct nx_catalog nx_catalog;

/* users_file_id / files_file_id are the page files attached to the pool. */
int nx_catalog_open(nx_bufpool *bp, uint32_t users_file_id, uint32_t users_pages,
                    uint32_t files_file_id, uint32_t files_pages, nx_catalog **out);
void nx_catalog_close(nx_catalog *c);

/* ---- apply (WAL redo and normal operation). Return 0 when applied, 1 when
 * there was nothing to do (file already added / already gone), or
 * NX_BP_FULL before changing anything: run a checkpoint and call again. -- */
int nx_catalog_put_user(nx_catalog *c, const nx_user *u, uint64_t lsn);
int nx_catalog_add_file(nx_catalog *c, const nx_file *f, uint64_t lsn);
int nx_catalog_set_file_flags(nx_catalog *c, uint64_t id, uint8_t flags, uint64_t lsn);
int nx_catalog_delete_file(nx_catalog *c, uint64_t id, uint64_t lsn);

/* ---- queries (copy out under the read lock) --------------------------- */
bool nx_catalog_user_by_id(nx_catalog *c, uint32_t id, nx_user *out);
bool nx_catalog_user_by_name(nx_catalog *c, const char *username, nx_user *out);
size_t nx_catalog_user_count(nx_catalog *c);
/* Copies all users into a malloc'd array. */
int nx_catalog_users(nx_catalog *c, nx_user **out, size_t *count);

bool nx_catalog_file_by_id(nx_catalog *c, uint64_t id, nx_file *out);
/* Newest file (highest id) of owner with this name, if any. */
bool nx_catalog_latest_version(nx_catalog *c, uint32_t owner, const char *name, nx_file *out);
/* Copies every file passing the filter (NULL = all) into a malloc'd array. */
typedef bool (*nx_file_filter)(const nx_file *f, void *ctx);
int nx_catalog_files(nx_catalog *c, nx_file_filter filter, void *ctx, nx_file **out, size_t *count);
size_t nx_catalog_file_count(nx_catalog *c);
uint64_t nx_catalog_max_file_id(nx_catalog *c);
uint32_t nx_catalog_max_user_id(nx_catalog *c);
uint32_t nx_catalog_page_count(nx_catalog *c, bool files);

#endif
