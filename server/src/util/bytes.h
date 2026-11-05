/*
 * bytes.h — little-endian, fixed-width serialization (CLAUDE.md "C rules":
 * on-disk structures are written field by field, never as raw structs, so
 * the format is identical on every compiler, CPU, and OS).
 */
#ifndef NX_BYTES_H
#define NX_BYTES_H

#include <stdint.h>

static inline void nx_put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static inline void nx_put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static inline void nx_put_u64(uint8_t *p, uint64_t v)
{
    nx_put_u32(p, (uint32_t)v);
    nx_put_u32(p + 4, (uint32_t)(v >> 32));
}

static inline uint16_t nx_get_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t nx_get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t nx_get_u64(const uint8_t *p)
{
    return (uint64_t)nx_get_u32(p) | ((uint64_t)nx_get_u32(p + 4) << 32);
}

#endif
