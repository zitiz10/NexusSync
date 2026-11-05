/* superblock.c — A/B superblock copies. See superblock.h. */
#include "engine/superblock.h"

#include "engine/chunk.h"
#include "platform/platform.h"
#include "util/bytes.h"
#include "util/crc32.h"
#include "util/failpoint.h"
#include "util/log.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define CRC_OFF 88u

void nx_superblock_init(nx_superblock *sb, uint64_t created_at)
{
    memset(sb, 0, sizeof *sb);
    sb->next_file_id = 1;
    sb->next_user_id = 1;
    sb->next_container_id = 1;
    sb->index_global_depth = 4;
    sb->cdc_min = NX_CDC_MIN;
    sb->cdc_avg = NX_CDC_AVG;
    sb->cdc_max = NX_CDC_MAX;
    sb->mask_s = NX_CDC_MASK_S;
    sb->mask_l = NX_CDC_MASK_L;
    sb->gear_seed_lo = (uint32_t)NX_GEAR_SEED;
    sb->gear_seed_hi = (uint32_t)(NX_GEAR_SEED >> 32);
    sb->created_at = created_at;
}

void nx_superblock_encode(const nx_superblock *sb, uint8_t out[NX_SUPERBLOCK_SIZE])
{
    memset(out, 0, NX_SUPERBLOCK_SIZE);
    memcpy(out, "NXSB", 4);
    nx_put_u16(out + 4, NX_SUPERBLOCK_VERSION);
    nx_put_u64(out + 8, sb->seq);
    nx_put_u64(out + 16, sb->checkpoint_lsn);
    nx_put_u64(out + 24, sb->next_file_id);
    nx_put_u32(out + 32, sb->next_user_id);
    nx_put_u32(out + 36, sb->next_container_id);
    nx_put_u32(out + 40, sb->active_container_id);
    nx_put_u32(out + 44, sb->index_global_depth);
    nx_put_u32(out + 48, sb->bucket_page_count);
    nx_put_u32(out + 52, sb->cdc_min);
    nx_put_u32(out + 56, sb->cdc_avg);
    nx_put_u32(out + 60, sb->cdc_max);
    nx_put_u32(out + 64, sb->mask_s);
    nx_put_u32(out + 68, sb->mask_l);
    nx_put_u32(out + 72, sb->gear_seed_lo);
    nx_put_u32(out + 76, sb->gear_seed_hi);
    nx_put_u64(out + 80, sb->created_at);
    nx_put_u32(out + CRC_OFF, nx_crc32(0, out, CRC_OFF));
}

int nx_superblock_decode(const uint8_t in[NX_SUPERBLOCK_SIZE], nx_superblock *sb)
{
    if (memcmp(in, "NXSB", 4) != 0 || nx_get_u16(in + 4) != NX_SUPERBLOCK_VERSION ||
        nx_get_u32(in + CRC_OFF) != nx_crc32(0, in, CRC_OFF))
        return -1;
    sb->seq = nx_get_u64(in + 8);
    sb->checkpoint_lsn = nx_get_u64(in + 16);
    sb->next_file_id = nx_get_u64(in + 24);
    sb->next_user_id = nx_get_u32(in + 32);
    sb->next_container_id = nx_get_u32(in + 36);
    sb->active_container_id = nx_get_u32(in + 40);
    sb->index_global_depth = nx_get_u32(in + 44);
    sb->bucket_page_count = nx_get_u32(in + 48);
    sb->cdc_min = nx_get_u32(in + 52);
    sb->cdc_avg = nx_get_u32(in + 56);
    sb->cdc_max = nx_get_u32(in + 60);
    sb->mask_s = nx_get_u32(in + 64);
    sb->mask_l = nx_get_u32(in + 68);
    sb->gear_seed_lo = nx_get_u32(in + 72);
    sb->gear_seed_hi = nx_get_u32(in + 76);
    sb->created_at = nx_get_u64(in + 80);
    return 0;
}

int nx_superblock_params_match(const nx_superblock *sb)
{
    return sb->cdc_min == NX_CDC_MIN && sb->cdc_avg == NX_CDC_AVG && sb->cdc_max == NX_CDC_MAX &&
           sb->mask_s == NX_CDC_MASK_S && sb->mask_l == NX_CDC_MASK_L &&
           sb->gear_seed_lo == (uint32_t)NX_GEAR_SEED &&
           sb->gear_seed_hi == (uint32_t)(NX_GEAR_SEED >> 32);
}

/* 0 valid, 1 missing, -1 present but invalid or unreadable. */
static int load_copy(const char *path, nx_superblock *sb)
{
    if (!plat_path_exists(path))
        return 1;
    plat_file *f = NULL;
    if (plat_file_open(path, PLAT_O_READ, &f) != 0)
        return -1;
    uint8_t buf[NX_SUPERBLOCK_SIZE];
    int64_t got = plat_pread(f, buf, sizeof buf, 0);
    if (plat_file_close(f) != 0 || got != (int64_t)sizeof buf)
        return -1;
    return nx_superblock_decode(buf, sb);
}

int nx_superblock_load(const char *data_dir, nx_superblock *out)
{
    char pa[1024], pb[1024];
    snprintf(pa, sizeof pa, "%s/superblock.a", data_dir);
    snprintf(pb, sizeof pb, "%s/superblock.b", data_dir);
    nx_superblock a, b;
    int ra = load_copy(pa, &a), rb = load_copy(pb, &b);
    if (ra == 1 && rb == 1)
        return 1;
    if (ra != 0 && rb != 0) {
        LOG_ERROR("superblock", "no valid superblock dir=%s", data_dir);
        return -1;
    }
    if (ra == 0 && (rb != 0 || a.seq > b.seq))
        *out = a;
    else
        *out = b;
    if (ra == -1 || rb == -1) /* a missing .b before the second write is normal */
        LOG_WARN("superblock", "one copy invalid, using the other seq=%" PRIu64, out->seq);
    return 0;
}

int nx_superblock_write(const char *data_dir, nx_superblock *sb)
{
    sb->seq++;
    char path[1024];
    snprintf(path, sizeof path, "%s/superblock.%c", data_dir, (sb->seq & 1u) ? 'a' : 'b');
    bool existed = plat_path_exists(path);
    uint8_t buf[NX_SUPERBLOCK_SIZE];
    nx_superblock_encode(sb, buf);
    plat_file *f = NULL;
    if (plat_file_open(path, PLAT_O_WRITE | PLAT_O_CREATE, &f) != 0)
        goto fail;
    bool already = nx_fail_crashed();
    if (nx_fail_hit("superblock.torn")) {
        /* Tear inside the fields (all meaningful bytes are in the first
         * 92), so the copy really is half old, half new. */
        if (!already)
            plat_pwrite_all(f, buf, 48, 0);
        plat_file_close(f);
        sb->seq--;
        return -1;
    }
    if (plat_pwrite_all(f, buf, sizeof buf, 0) != 0 || plat_fsync_file(f) != 0) {
        plat_file_close(f);
        goto fail;
    }
    if (plat_file_close(f) != 0)
        goto fail;
    if (!existed && plat_fsync_dir(data_dir) != 0)
        goto fail;
    return 0;
fail:
    LOG_ERROR("superblock", "write failed path=%s err=%s", path, plat_last_error());
    sb->seq--;
    return -1;
}
