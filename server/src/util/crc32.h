/*
 * crc32.h — CRC-32 (IEEE 802.3, reflected polynomial 0xEDB88320), the same
 * checksum as zlib.crc32. Detects torn and corrupted records on disk
 * (WAL records, pages, container blocks, superblock).
 */
#ifndef NX_CRC32_H
#define NX_CRC32_H

#include <stddef.h>
#include <stdint.h>

/* Start with crc = 0. Feeding data in pieces gives the same result as one
 * call: nx_crc32(nx_crc32(0, a, n), b, m) == nx_crc32(0, a||b, n+m). */
uint32_t nx_crc32(uint32_t crc, const void *data, size_t len);

#endif
