/*
 * container.h — append-only block containers (SPEC §5, §7.1).
 *
 * File data/containers/cNNNNNNNN.dat:
 *   header (64 bytes): magic "NXSC", u16 version, u16 0, u32 id,
 *                      u64 created_at, zero padding
 *   records:           u8 hash[32], u32 len, u32 crc32(data), data[len]
 *
 * One container is "active" and receives appends; when the next record
 * would exceed container_size it is sealed (fsynced) and a new one starts.
 * The block hash is stored inline so the index can be rebuilt from the
 * containers alone (--rebuild-index).
 *
 * The active container is always the one with the highest id on disk. If
 * its tail is damaged (a crash in the middle of an append), we do not
 * append after the damage: we seal it as it is and start a new container,
 * so a sequential scan never has to skip over garbage between records.
 */
#ifndef NX_CONTAINER_H
#define NX_CONTAINER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NX_CONTAINER_HEADER 64u
#define NX_RECORD_HEADER 40u /* hash 32 + len 4 + crc 4 */
#define NX_CONTAINER_VERSION 1u

typedef struct nx_containers nx_containers;

typedef struct {
    uint32_t container_id;
    uint32_t offset; /* of the record header */
    uint32_t len;    /* data bytes */
} nx_block_loc;

/* Opens dir (creating it and container 1 if empty). container_size is the
 * seal threshold in bytes (SPEC §3, default 4 MB). */
int nx_containers_open(const char *dir, uint64_t container_size, nx_containers **out);
void nx_containers_close(nx_containers *c);

/* Appends one record to the active container (thread-safe). Not durable
 * until nx_containers_sync returns. */
int nx_containers_append(nx_containers *c, const uint8_t hash[32], const uint8_t *data,
                         uint32_t len, nx_block_loc *loc);

/* Makes every append so far durable (fsync of the active container; sealed
 * containers were fsynced when sealed). */
int nx_containers_sync(nx_containers *c);

/* Reads a record's data into buf (cap >= loc->len). Checks that the stored
 * length matches and the CRC is right. If hash_out is given, copies the
 * stored hash. Returns 0, or -1 with *corrupt set if the bytes are damaged. */
int nx_containers_read(nx_containers *c, const nx_block_loc *loc, uint8_t *buf, size_t cap,
                       uint8_t hash_out[32], bool *corrupt);

uint32_t nx_containers_active_id(nx_containers *c);

/* Visits every record of one container in order. cb gets the location,
 * stored hash, and whether its CRC verified; if data_out is non-NULL the
 * data is passed too. Stops at the first unparsable record and reports the
 * bytes after it in *trailing_bytes (an interrupted append). */
typedef int (*nx_record_cb)(const nx_block_loc *loc, const uint8_t hash[32], bool crc_ok,
                            const uint8_t *data, void *ctx);
int nx_containers_scan(nx_containers *c, uint32_t id, bool with_data, nx_record_cb cb, void *ctx,
                       uint64_t *trailing_bytes);

/* Lists existing container ids in ascending order (malloc'd array). */
int nx_containers_list(nx_containers *c, uint32_t **ids, size_t *count);

/* Size of a container file in bytes (for stats and compaction). */
int nx_containers_file_size(nx_containers *c, uint32_t id, uint64_t *size);

/* Seals the active container now and starts a new one (compaction uses a
 * fresh container). Returns the new active id. */
int nx_containers_rotate(nx_containers *c, uint32_t *new_id);

/* Deletes a container file (after compaction moved its live blocks). */
int nx_containers_drop(nx_containers *c, uint32_t id);

#endif
