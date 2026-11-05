/* wal.c — write-ahead log segments. See wal.h. */
#include "engine/wal.h"

#include "engine/logrec.h"
#include "platform/platform.h"
#include "util/bytes.h"
#include "util/crc32.h"
#include "util/failpoint.h"
#include "util/log.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t seq;
    uint64_t first_lsn;
} segment;

struct nx_wal {
    pthread_mutex_t mu;
    char dir[1024];
    uint64_t seg_size;
    segment *segs; /* ascending seq; the last one is active */
    size_t nsegs, cap;
    plat_file *active;
    uint64_t append_off;
    uint64_t next_lsn;
    uint64_t bytes_before_active; /* sizes of the older segments, for stats */
    bool recovered;
    bool broken;
};

static void seg_path(const nx_wal *w, uint32_t seq, char *buf, size_t n)
{
    snprintf(buf, n, "%s/wal.%08" PRIu32, w->dir, seq);
}

static void encode_header(uint8_t h[NX_WAL_SEG_HEADER], uint64_t first_lsn)
{
    memcpy(h, "NXWL", 4);
    nx_put_u16(h + 4, NX_WAL_VERSION);
    nx_put_u16(h + 6, 0);
    nx_put_u64(h + 8, first_lsn);
}

static int push_seg(nx_wal *w, uint32_t seq, uint64_t first_lsn)
{
    if (w->nsegs == w->cap) {
        size_t cap = w->cap ? w->cap * 2 : 8;
        segment *s = realloc(w->segs, cap * sizeof *s);
        if (!s)
            return -1;
        w->segs = s;
        w->cap = cap;
    }
    w->segs[w->nsegs++] = (segment){seq, first_lsn};
    return 0;
}

/* Creates segment seq with a durable header and makes it the active one. */
static int create_segment(nx_wal *w, uint32_t seq, uint64_t first_lsn)
{
    char path[1100];
    seg_path(w, seq, path, sizeof path);
    plat_file *f;
    if (plat_file_open(path, PLAT_O_WRITE | PLAT_O_CREATE | PLAT_O_TRUNC, &f) != 0)
        return -1;
    uint8_t h[NX_WAL_SEG_HEADER];
    encode_header(h, first_lsn);
    if (plat_pwrite_all(f, h, sizeof h, 0) != 0 || plat_fsync_file(f) != 0 ||
        plat_fsync_dir(w->dir) != 0) {
        plat_file_close(f);
        return -1;
    }
    if (w->active) {
        w->bytes_before_active += w->append_off;
        plat_file_close(w->active);
    }
    w->active = f;
    w->append_off = NX_WAL_SEG_HEADER;
    return push_seg(w, seq, first_lsn);
}

static int list_cb(const char *name, bool is_dir, void *ctx)
{
    nx_wal *w = ctx;
    unsigned long v;
    char extra;
    if (is_dir || strncmp(name, "wal.", 4) != 0 || strlen(name) != 12 ||
        sscanf(name + 4, "%8lu%c", &v, &extra) != 1 || v == 0)
        return 0;
    return push_seg(w, (uint32_t)v, 0);
}

static int cmp_seg(const void *a, const void *b)
{
    uint32_t x = ((const segment *)a)->seq, y = ((const segment *)b)->seq;
    return x < y ? -1 : x > y;
}

int nx_wal_open(const char *dir, uint64_t segment_size, uint64_t first_lsn, nx_wal **out)
{
    nx_wal *w = calloc(1, sizeof *w);
    if (!w)
        return -1;
    snprintf(w->dir, sizeof w->dir, "%s", dir);
    w->seg_size = segment_size < 4096 ? 4096 : segment_size;
    pthread_mutex_init(&w->mu, NULL);
    if (plat_mkdir_p(dir) != 0 || plat_list_dir(dir, list_cb, w) < 0)
        goto fail;
    if (w->nsegs == 0) {
        if (create_segment(w, 1, first_lsn) != 0)
            goto fail;
        w->next_lsn = first_lsn;
    } else {
        qsort(w->segs, w->nsegs, sizeof *w->segs, cmp_seg);
    }
    *out = w;
    return 0;
fail:
    LOG_ERROR("wal", "open failed dir=%s err=%s", dir, plat_last_error());
    nx_wal_close(w);
    return -1;
}

void nx_wal_close(nx_wal *w)
{
    if (!w)
        return;
    if (w->active && plat_file_close(w->active) != 0)
        LOG_ERROR("wal", "close failed err=%s", plat_last_error());
    free(w->segs);
    pthread_mutex_destroy(&w->mu);
    free(w);
}

static int read_file(const char *path, uint8_t **buf, uint64_t *len)
{
    plat_file *f;
    *buf = NULL;
    if (plat_file_open(path, PLAT_O_READ, &f) != 0)
        return -1;
    int rc = plat_file_size(f, len);
    if (rc == 0 && *len > SIZE_MAX - 1)
        rc = -1;
    if (rc == 0) {
        *buf = malloc((size_t)*len + 1);
        if (!*buf || plat_pread(f, *buf, (size_t)*len, 0) != (int64_t)*len)
            rc = -1;
    }
    if (plat_file_close(f) != 0)
        rc = -1;
    if (rc != 0) {
        free(*buf);
        *buf = NULL;
    }
    return rc;
}

/* Parses one record at buf[off..len). Returns its total size, or 0 if the
 * bytes there are not a complete, valid record. */
static size_t parse_record(const uint8_t *buf, uint64_t len, uint64_t off, nx_wal_rec *r)
{
    if (len - off < NX_WAL_REC_OVERHEAD)
        return 0;
    const uint8_t *p = buf + off;
    uint32_t plen = nx_get_u32(p + 17);
    if (plen > NX_WAL_MAX_PAYLOAD || len - off < NX_WAL_REC_OVERHEAD + (uint64_t)plen)
        return 0;
    size_t total = NX_WAL_REC_OVERHEAD + plen;
    if (nx_get_u32(p + total - 4) != nx_crc32(0, p, total - 4))
        return 0;
    r->lsn = nx_get_u64(p);
    r->txn_id = nx_get_u64(p + 8);
    r->type = p[16];
    r->len = plen;
    r->payload = p + 21;
    return total;
}

int nx_wal_recover(nx_wal *w, uint64_t from_lsn, nx_wal_cb cb, void *ctx, nx_wal_recovery *out)
{
    memset(out, 0, sizeof *out);
    pthread_mutex_lock(&w->mu);
    int rc = 0;
    /* Where the log is cut afterwards: end of the last COMMIT/CHECKPOINT. */
    size_t safe_seg = 0;
    uint64_t safe_off = NX_WAL_SEG_HEADER, safe_lsn = 0, prev_lsn = 0;
    bool have_prev = false, stop = false;
    uint64_t total_bytes = 0;
    for (size_t i = 0; i < w->nsegs && rc == 0 && !stop; i++) {
        bool last = i + 1 == w->nsegs;
        char path[1100];
        seg_path(w, w->segs[i].seq, path, sizeof path);
        uint8_t *buf;
        uint64_t len;
        if (read_file(path, &buf, &len) != 0) {
            rc = -1;
            break;
        }
        total_bytes += len;
        if (len < NX_WAL_SEG_HEADER || memcmp(buf, "NXWL", 4) != 0 ||
            nx_get_u16(buf + 4) != NX_WAL_VERSION) {
            free(buf);
            if (!last) {
                LOG_ERROR("wal", "segment header damaged seq=%" PRIu32, w->segs[i].seq);
                rc = -1;
                break;
            }
            /* The newest segment's header never became durable: nothing
             * was committed in it. It is rewritten below. */
            w->segs[i].first_lsn = 0;
            stop = true;
            break;
        }
        w->segs[i].first_lsn = nx_get_u64(buf + 8);
        if (i == 0 && !have_prev) {
            /* An empty first segment: the next record will carry first_lsn. */
            safe_lsn = w->segs[0].first_lsn ? w->segs[0].first_lsn - 1 : 0;
        }
        uint64_t off = NX_WAL_SEG_HEADER;
        while (off < len) {
            nx_wal_rec r;
            size_t n = parse_record(buf, len, off, &r);
            /* LSNs are consecutive, and a segment starts at its first_lsn. */
            bool ok = n > 0 && (!have_prev || r.lsn == prev_lsn + 1) &&
                      (off != NX_WAL_SEG_HEADER || r.lsn == w->segs[i].first_lsn);
            if (!ok) {
                if (!last) {
                    LOG_ERROR("wal", "damaged record inside an older segment seq=%" PRIu32
                              " offset=%" PRIu64 "; refusing to drop committed work",
                              w->segs[i].seq, off);
                    rc = -1;
                }
                stop = true;
                break;
            }
            prev_lsn = r.lsn;
            have_prev = true;
            off += n;
            if (r.lsn > from_lsn) {
                out->records_read++;
                if (cb && cb(&r, ctx) != 0) {
                    rc = -1;
                    break;
                }
            }
            if (r.type == NX_REC_COMMIT || r.type == NX_REC_CHECKPOINT || r.lsn <= from_lsn) {
                safe_seg = i;
                safe_off = off;
                safe_lsn = r.lsn;
            }
        }
        free(buf);
    }
    if (rc != 0) {
        pthread_mutex_unlock(&w->mu);
        return rc;
    }

    /* Cut everything after (safe_seg, safe_off). */
    char path[1100];
    uint64_t kept_bytes = 0;
    for (size_t i = w->nsegs; i-- > safe_seg + 1;) {
        seg_path(w, w->segs[i].seq, path, sizeof path);
        if (plat_remove(path) != 0) {
            rc = -1;
            break;
        }
        w->nsegs--;
    }
    if (rc == 0 && w->nsegs != safe_seg + 1)
        rc = -1;
    if (rc == 0 && plat_fsync_dir(w->dir) != 0)
        rc = -1;
    if (rc == 0) {
        seg_path(w, w->segs[safe_seg].seq, path, sizeof path);
        if (plat_file_open(path, PLAT_O_WRITE | PLAT_O_CREATE, &w->active) != 0)
            rc = -1;
    }
    if (rc == 0 && safe_off > NX_WAL_SEG_HEADER && safe_lsn < from_lsn) {
        /* The superblock says changes up to from_lsn were checkpointed, so
         * the log must reach at least that far. */
        LOG_ERROR("wal", "log ends at lsn=%" PRIu64 " before the checkpoint lsn=%" PRIu64, safe_lsn,
                  from_lsn);
        rc = -1;
    }
    if (rc == 0) {
        /* Never hand out an LSN at or below the checkpoint: pages may carry
         * LSNs up to it, and redo compares against them. */
        w->next_lsn = (safe_lsn > from_lsn ? safe_lsn : from_lsn) + 1;
        if (safe_off == NX_WAL_SEG_HEADER) {
            /* Nothing kept in this segment: give it a correct header. */
            w->segs[safe_seg].first_lsn = w->next_lsn;
            uint8_t h[NX_WAL_SEG_HEADER];
            encode_header(h, w->next_lsn);
            if (plat_pwrite_all(w->active, h, sizeof h, 0) != 0)
                rc = -1;
        }
        if (rc == 0 && (plat_file_truncate(w->active, safe_off) != 0 ||
                        plat_fsync_file(w->active) != 0))
            rc = -1;
    }
    if (rc == 0) {
        for (size_t i = 0; i < safe_seg; i++) {
            uint64_t sz = 0;
            plat_file *f;
            seg_path(w, w->segs[i].seq, path, sizeof path);
            if (plat_file_open(path, PLAT_O_READ, &f) == 0) {
                plat_file_size(f, &sz);
                plat_file_close(f);
            }
            kept_bytes += sz;
        }
        w->bytes_before_active = kept_bytes;
        w->append_off = safe_off;
        out->bytes_cut = total_bytes > kept_bytes + safe_off ? total_bytes - kept_bytes - safe_off : 0;
        out->last_lsn = safe_lsn;
        w->recovered = true;
        if (out->bytes_cut)
            LOG_WARN("wal", "cut torn or uncommitted tail bytes=%" PRIu64 " next_lsn=%" PRIu64,
                     out->bytes_cut, w->next_lsn);
    } else {
        LOG_ERROR("wal", "recovery could not cut the log err=%s", plat_last_error());
    }
    pthread_mutex_unlock(&w->mu);
    return rc;
}

int nx_wal_append(nx_wal *w, const nx_wal_rec *recs, size_t n, bool sync)
{
    if (n == 0)
        return 0;
    size_t total = 0;
    for (size_t i = 0; i < n; i++) {
        if (recs[i].len > NX_WAL_MAX_PAYLOAD)
            return -1;
        total += NX_WAL_REC_OVERHEAD + recs[i].len;
    }
    uint8_t *buf = malloc(total);
    if (!buf)
        return -1;
    pthread_mutex_lock(&w->mu);
    int rc = 0;
    if (!w->recovered || w->broken) {
        LOG_ERROR("wal", "append refused: log %s", w->broken ? "closed after a failed write"
                                                                : "not recovered yet");
        pthread_mutex_unlock(&w->mu);
        free(buf);
        return -1;
    }
    if (recs[0].lsn != w->next_lsn) {
        LOG_ERROR("wal", "append refused lsn=%" PRIu64 " expected=%" PRIu64, recs[0].lsn,
                  w->next_lsn);
        rc = -1;
        goto out;
    }
    uint8_t *p = buf;
    for (size_t i = 0; i < n; i++) {
        if (recs[i].lsn != w->next_lsn + i) {
            rc = -1;
            goto out;
        }
        nx_put_u64(p, recs[i].lsn);
        nx_put_u64(p + 8, recs[i].txn_id);
        p[16] = recs[i].type;
        nx_put_u32(p + 17, recs[i].len);
        if (recs[i].len)
            memcpy(p + 21, recs[i].payload, recs[i].len);
        size_t rl = NX_WAL_REC_OVERHEAD + recs[i].len;
        nx_put_u32(p + rl - 4, nx_crc32(0, p, rl - 4));
        p += rl;
    }
    /* Crash points fire on commit appends (sync), so tests can count
     * transactions; after a crash nothing at all is written. */
    if (nx_fail_crashed() || (sync && nx_fail_hit("wal.append"))) {
        rc = -1;
        goto out;
    }
    /* Never split a transaction across segments; the old segment is
     * complete and fsynced before the new one starts. */
    if (w->append_off + total > w->seg_size && w->append_off > NX_WAL_SEG_HEADER) {
        if (plat_fsync_file(w->active) != 0 ||
            create_segment(w, w->segs[w->nsegs - 1].seq + 1, recs[0].lsn) != 0) {
            rc = -1;
            goto out;
        }
    }
    bool already = nx_fail_crashed();
    if (sync && nx_fail_hit("wal.append.torn")) {
        if (!already) /* the crash happens in the middle of this write */
            plat_pwrite_all(w->active, buf, total / 2, w->append_off);
        rc = -1;
        goto out;
    }
    if (plat_pwrite_all(w->active, buf, total, w->append_off) != 0) {
        rc = -1;
        goto out;
    }
    if (sync && nx_fail_hit("wal.before_fsync")) {
        rc = -1;
        goto out;
    }
    if (sync && plat_fsync_file(w->active) != 0) {
        rc = -1;
        goto out;
    }
    w->append_off += total;
    w->next_lsn += n;
out:
    if (rc != 0) {
        /* The file may now end in a partial write. Appending after it would
         * hide later records behind garbage, so stop; restart recovery cuts
         * the tail. */
        w->broken = true;
        if (!nx_fail_crashed())
            LOG_ERROR("wal", "append failed, log closed for writing err=%s", plat_last_error());
    }
    pthread_mutex_unlock(&w->mu);
    free(buf);
    return rc;
}

uint64_t nx_wal_next_lsn(nx_wal *w)
{
    pthread_mutex_lock(&w->mu);
    uint64_t v = w->next_lsn;
    pthread_mutex_unlock(&w->mu);
    return v;
}

int nx_wal_truncate_below(nx_wal *w, uint64_t lsn)
{
    pthread_mutex_lock(&w->mu);
    size_t drop = 0;
    /* Segment i holds LSNs [first_lsn(i), first_lsn(i+1)). */
    while (drop + 1 < w->nsegs && w->segs[drop + 1].first_lsn <= lsn + 1)
        drop++;
    int rc = 0;
    size_t done = 0;
    for (; done < drop; done++) {
        char path[1100];
        seg_path(w, w->segs[done].seq, path, sizeof path);
        plat_file *f;
        uint64_t sz = 0;
        if (plat_file_open(path, PLAT_O_READ, &f) == 0) {
            plat_file_size(f, &sz);
            plat_file_close(f);
        }
        if (plat_remove(path) != 0) {
            rc = -1;
            break;
        }
        w->bytes_before_active -= sz <= w->bytes_before_active ? sz : w->bytes_before_active;
    }
    if (done) {
        memmove(w->segs, w->segs + done, (w->nsegs - done) * sizeof *w->segs);
        w->nsegs -= done;
        if (plat_fsync_dir(w->dir) != 0)
            rc = -1;
    }
    pthread_mutex_unlock(&w->mu);
    return rc;
}

void nx_wal_get_stats(nx_wal *w, nx_wal_stats *out)
{
    pthread_mutex_lock(&w->mu);
    out->segments = (uint32_t)w->nsegs;
    out->bytes = w->bytes_before_active + w->append_off;
    pthread_mutex_unlock(&w->mu);
}
