/* checkpoint.c — doublewrite batches. See checkpoint.h. */
#include "engine/checkpoint.h"

#include "platform/platform.h"
#include "util/bytes.h"
#include "util/crc32.h"
#include "util/failpoint.h"
#include "util/log.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

int nx_dw_write(const char *path, const nx_dw_page *pages, size_t n, uint64_t checkpoint_lsn)
{
    size_t total = NX_DW_HEADER + n * NX_DW_ENTRY + NX_DW_TRAILER;
    uint8_t *buf = malloc(total);
    if (!buf)
        return -1;
    uint8_t *p = buf;
    memcpy(p, "NXDW", 4);
    nx_put_u16(p + 4, NX_DW_VERSION);
    nx_put_u16(p + 6, 0);
    nx_put_u64(p + 8, checkpoint_lsn);
    p += NX_DW_HEADER;
    for (size_t i = 0; i < n; i++, p += NX_DW_ENTRY) {
        nx_put_u32(p, pages[i].file_id);
        nx_put_u32(p + 4, pages[i].page_no);
        memcpy(p + 8, pages[i].data, 4096);
        nx_put_u32(p + 8 + 4096, nx_crc32(0, p, 8 + 4096));
    }
    memcpy(p, "NXDT", 4);
    nx_put_u32(p + 4, (uint32_t)n);
    nx_put_u32(p + 8, nx_crc32(0, buf, total - 4));

    plat_file *f = NULL;
    int rc = -1;
    bool already = nx_fail_crashed();
    if (nx_fail_hit("doublewrite.torn")) {
        if (!already && plat_file_open(path, PLAT_O_WRITE | PLAT_O_CREATE, &f) == 0) {
            plat_pwrite_all(f, buf, total / 2, 0);
            plat_file_close(f);
        }
        free(buf);
        return -1;
    }
    if (plat_file_open(path, PLAT_O_WRITE | PLAT_O_CREATE, &f) == 0) {
        rc = plat_pwrite_all(f, buf, total, 0);
        if (rc == 0)
            rc = plat_file_truncate(f, total);
        if (rc == 0)
            rc = plat_fsync_file(f);
        if (plat_file_close(f) != 0)
            rc = -1;
    }
    if (rc != 0)
        LOG_ERROR("checkpoint", "doublewrite failed path=%s err=%s", path, plat_last_error());
    free(buf);
    return rc;
}

int nx_dw_load(const char *path, nx_dw_page **pages, uint8_t **store, size_t *n,
               uint64_t *checkpoint_lsn)
{
    *pages = NULL;
    *store = NULL;
    *n = 0;
    *checkpoint_lsn = 0;
    if (!plat_path_exists(path))
        return 0;
    plat_file *f = NULL;
    if (plat_file_open(path, PLAT_O_READ, &f) != 0)
        return -1;
    uint64_t size = 0;
    if (plat_file_size(f, &size) != 0) {
        plat_file_close(f);
        return -1;
    }
    if (size < NX_DW_HEADER + NX_DW_TRAILER || (size - NX_DW_HEADER - NX_DW_TRAILER) % NX_DW_ENTRY ||
        size > SIZE_MAX) {
        plat_file_close(f);
        return 0; /* empty or incomplete: the in-place writes never started */
    }
    uint8_t *buf = malloc((size_t)size);
    if (!buf) {
        plat_file_close(f);
        return -1;
    }
    int64_t got = plat_pread(f, buf, (size_t)size, 0);
    if (plat_file_close(f) != 0 || got < 0) {
        free(buf);
        return -1;
    }
    size_t count = (size_t)((size - NX_DW_HEADER - NX_DW_TRAILER) / NX_DW_ENTRY);
    const uint8_t *tr = buf + size - NX_DW_TRAILER;
    bool ok = (uint64_t)got == size && memcmp(buf, "NXDW", 4) == 0 &&
              nx_get_u16(buf + 4) == NX_DW_VERSION && memcmp(tr, "NXDT", 4) == 0 &&
              nx_get_u32(tr + 4) == count &&
              nx_get_u32(tr + 8) == nx_crc32(0, buf, (size_t)size - 4);
    nx_dw_page *pv = NULL;
    if (ok && count) {
        pv = malloc(count * sizeof *pv);
        if (!pv) {
            free(buf);
            return -1;
        }
        const uint8_t *p = buf + NX_DW_HEADER;
        for (size_t i = 0; i < count && ok; i++, p += NX_DW_ENTRY) {
            if (nx_get_u32(p + 8 + 4096) != nx_crc32(0, p, 8 + 4096))
                ok = false;
            pv[i] = (nx_dw_page){nx_get_u32(p), nx_get_u32(p + 4), p + 8};
        }
    }
    if (!ok || count == 0) {
        if (!ok)
            LOG_WARN("checkpoint", "doublewrite batch incomplete, ignored bytes=%" PRIu64, size);
        free(pv);
        free(buf);
        return 0;
    }
    *checkpoint_lsn = nx_get_u64(buf + 8);
    *pages = pv;
    *store = buf;
    *n = count;
    return 0;
}

int nx_dw_clear(const char *path)
{
    plat_file *f = NULL;
    if (plat_file_open(path, PLAT_O_WRITE | PLAT_O_CREATE, &f) != 0)
        return -1;
    int rc = plat_file_truncate(f, 0);
    if (rc == 0)
        rc = plat_fsync_file(f);
    if (plat_file_close(f) != 0)
        rc = -1;
    return rc;
}
