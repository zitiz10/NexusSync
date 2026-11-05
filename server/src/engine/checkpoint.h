/*
 * checkpoint.h — the doublewrite file used by checkpoints (SPEC §8).
 *
 * Why: a checkpoint overwrites pages in place. If the machine dies in the
 * middle of writing a 4 KB page, the page on disk is half old, half new
 * (a "torn page"), and its CRC fails: the data is gone. So every page is
 * first written, together with its destination, to doublewrite.bin and
 * fsynced. Only then are the pages written in place. After a crash, a
 * complete doublewrite batch is simply copied in place again, which
 * repairs any torn page; an incomplete batch means the in-place writes
 * never started, so the old pages are intact.
 *
 * doublewrite.bin:
 *   header 16 bytes: "NXDW", u16 version, u16 0, u64 checkpoint_lsn
 *   entries:         u32 file_id, u32 page_no, page[4096],
 *                    u32 crc32 over the entry's preceding 4104 bytes
 *   trailer 12 bytes: "NXDT", u32 count, u32 crc32 over every byte before
 *                     this crc field (header, entries, "NXDT", count)
 * "Empty" means length 0.
 */
#ifndef NX_CHECKPOINT_H
#define NX_CHECKPOINT_H

#include <stddef.h>
#include <stdint.h>

#define NX_DW_VERSION 1u
#define NX_DW_HEADER 16u
#define NX_DW_ENTRY (4u + 4u + 4096u + 4u)
#define NX_DW_TRAILER 12u

typedef struct {
    uint32_t file_id;
    uint32_t page_no;
    const uint8_t *data; /* 4096 bytes */
} nx_dw_page;

/* Writes the whole batch and fsyncs it. */
int nx_dw_write(const char *path, const nx_dw_page *pages, size_t n, uint64_t checkpoint_lsn);

/* Loads a complete, valid batch. *n is 0 when the file is missing, empty,
 * or holds an incomplete/damaged batch (nothing to repair). On success with
 * *n > 0, *pages and *store are malloc'd (free both). Returns -1 only on I/O
 * or memory failure. */
int nx_dw_load(const char *path, nx_dw_page **pages, uint8_t **store, size_t *n,
               uint64_t *checkpoint_lsn);

/* Marks the file empty (truncate to 0) and fsyncs it. */
int nx_dw_clear(const char *path);

#endif
