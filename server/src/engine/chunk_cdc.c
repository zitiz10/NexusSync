/*
 * chunk_cdc.c — content-defined chunking, FastCDC style (SPEC §6.2).
 *
 * The Gear hash: h = (h << 1) + GEAR[byte]. Shifting left by one each step
 * means a byte's influence leaves the 32-bit hash after 32 steps, so h
 * depends only on the last 32 bytes: a rolling hash with no explicit
 * window. We cut where the top bits of h are all zero.
 *
 * "Normalized chunking": before AVG we test 14 bits (cut probability
 * 1/16384 per byte, so cuts are rare), after AVG only 10 bits (1/1024, so
 * cuts come soon). This pulls block sizes towards AVG. No cut is allowed
 * before MIN, and MAX forces one.
 *
 * Because the boundary depends only on nearby content, inserting bytes
 * into a file shifts later boundaries along with the data. The blocks
 * after the edit are byte-identical to before and deduplicate.
 */
#include "engine/chunk.h"
#include "engine/gear_table.h"

size_t nx_cdc_cut(const uint8_t *buf, size_t n)
{
    if (n <= NX_CDC_MIN)
        return n;
    size_t normal = n < NX_CDC_AVG ? n : NX_CDC_AVG;
    size_t limit = n < NX_CDC_MAX ? n : NX_CDC_MAX;
    uint32_t h = 0;
    size_t i = NX_CDC_MIN;
    for (; i < normal; i++) {
        h = (h << 1) + NX_GEAR[buf[i]];
        if ((h & NX_CDC_MASK_S) == 0)
            return i + 1;
    }
    for (; i < limit; i++) {
        h = (h << 1) + NX_GEAR[buf[i]];
        if ((h & NX_CDC_MASK_L) == 0)
            return i + 1;
    }
    return limit;
}
