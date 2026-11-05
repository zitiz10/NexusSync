/* chunk_fixed.c — fixed-size blocks (SPEC §6.1). Used as a baseline: the
 * Compare screen shows why fixed blocks stop matching after an insertion. */
#include "engine/chunk.h"

size_t nx_fixed_cut(const uint8_t *buf, size_t n)
{
    (void)buf;
    return n < NX_FIXED_SIZE ? n : NX_FIXED_SIZE;
}
