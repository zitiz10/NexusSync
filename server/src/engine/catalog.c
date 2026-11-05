/* catalog.c — users and files in slotted pages. See catalog.h. */
#include "engine/catalog.h"

#include "util/bytes.h"
#include "util/hmap.h"
#include "util/log.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define SP_HEADER 16u
#define SP_SLOT 4u

/* ---- record encoding ------------------------------------------------------ */

size_t nx_user_encode(const nx_user *u, uint8_t *out)
{
    size_t ul = strlen(u->username), dl = strlen(u->display), p = 0;
    nx_put_u32(out + p, u->id), p += 4;
    out[p++] = u->role;
    out[p++] = u->status;
    nx_put_u16(out + p, (uint16_t)ul), p += 2;
    memcpy(out + p, u->username, ul), p += ul;
    nx_put_u16(out + p, (uint16_t)dl), p += 2;
    memcpy(out + p, u->display, dl), p += dl;
    memcpy(out + p, u->salt, 16), p += 16;
    memcpy(out + p, u->pw_hash, 32), p += 32;
    nx_put_u32(out + p, u->pbkdf2_iters), p += 4;
    nx_put_u64(out + p, u->created_at), p += 8;
    nx_put_u64(out + p, u->last_seen), p += 8;
    return p;
}

/* Bounds-checked reader over a byte buffer. */
typedef struct {
    const uint8_t *p;
    size_t len, pos;
    bool bad;
} rd;

static const uint8_t *take(rd *r, size_t n)
{
    if (r->bad || r->len - r->pos < n) {
        r->bad = true;
        return NULL;
    }
    const uint8_t *q = r->p + r->pos;
    r->pos += n;
    return q;
}
static uint8_t rd8(rd *r) { const uint8_t *q = take(r, 1); return q ? q[0] : 0; }
static uint16_t rd16(rd *r) { const uint8_t *q = take(r, 2); return q ? nx_get_u16(q) : 0; }
static uint32_t rd32(rd *r) { const uint8_t *q = take(r, 4); return q ? nx_get_u32(q) : 0; }
static uint64_t rd64(rd *r) { const uint8_t *q = take(r, 8); return q ? nx_get_u64(q) : 0; }

static void rdstr(rd *r, char *dst, size_t max)
{
    uint16_t n = rd16(r);
    if (n > max) {
        r->bad = true;
        return;
    }
    const uint8_t *q = take(r, n);
    if (!q)
        return;
    memcpy(dst, q, n);
    dst[n] = '\0';
    if (memchr(dst, '\0', n)) /* embedded NUL: refuse */
        r->bad = true;
}

int nx_user_decode(const uint8_t *in, size_t len, nx_user *u)
{
    rd r = {in, len, 0, false};
    memset(u, 0, sizeof *u);
    u->id = rd32(&r);
    u->role = rd8(&r);
    u->status = rd8(&r);
    rdstr(&r, u->username, NX_USERNAME_MAX);
    rdstr(&r, u->display, NX_DISPLAY_MAX);
    const uint8_t *s = take(&r, 16), *h = take(&r, 32);
    if (s)
        memcpy(u->salt, s, 16);
    if (h)
        memcpy(u->pw_hash, h, 32);
    u->pbkdf2_iters = rd32(&r);
    u->created_at = rd64(&r);
    u->last_seen = rd64(&r);
    if (r.bad)
        return -1;
    /* Stored user records are zero-padded to NX_USER_REC_MAX so updates are
     * always in place; anything after the fields must be padding. */
    for (size_t i = r.pos; i < len; i++)
        if (in[i] != 0)
            return -1;
    return 0;
}

size_t nx_file_encode(const nx_file *f, uint8_t *out)
{
    size_t nl = strlen(f->name), p = 0;
    nx_put_u64(out + p, f->id), p += 8;
    nx_put_u32(out + p, f->owner_id), p += 4;
    nx_put_u16(out + p, (uint16_t)nl), p += 2;
    memcpy(out + p, f->name, nl), p += nl;
    nx_put_u32(out + p, f->version), p += 4;
    nx_put_u64(out + p, f->prev_version_id), p += 8;
    nx_put_u64(out + p, f->size), p += 8;
    nx_put_u32(out + p, f->chunk_count), p += 4;
    out[p++] = f->chunker;
    nx_put_u64(out + p, f->recipe_offset), p += 8;
    nx_put_u32(out + p, f->recipe_len), p += 4;
    memcpy(out + p, f->sha256, 32), p += 32;
    nx_put_u64(out + p, f->bytes_sent), p += 8;
    nx_put_u64(out + p, f->created_at), p += 8;
    out[p++] = f->flags;
    return p;
}

int nx_file_decode(const uint8_t *in, size_t len, nx_file *f)
{
    rd r = {in, len, 0, false};
    memset(f, 0, sizeof *f);
    f->id = rd64(&r);
    f->owner_id = rd32(&r);
    rdstr(&r, f->name, NX_FILENAME_MAX);
    f->version = rd32(&r);
    f->prev_version_id = rd64(&r);
    f->size = rd64(&r);
    f->chunk_count = rd32(&r);
    f->chunker = rd8(&r);
    f->recipe_offset = rd64(&r);
    f->recipe_len = rd32(&r);
    const uint8_t *h = take(&r, 32);
    if (h)
        memcpy(f->sha256, h, 32);
    f->bytes_sent = rd64(&r);
    f->created_at = rd64(&r);
    f->flags = rd8(&r);
    return r.bad || r.pos != len ? -1 : 0;
}

/* ---- slotted-page table ------------------------------------------------------- */

typedef struct {
    nx_bufpool *bp;
    uint32_t file_id;
    uint32_t pages;
    uint16_t *free_total; /* per page: contiguous free + holes of deleted records */
    size_t free_cap;
} table;

static uint16_t sp_slots(const uint8_t *p) { return nx_get_u16(p + 8); }
static uint16_t sp_free_off(const uint8_t *p) { return nx_get_u16(p + 10); }
static void sp_set_slots(uint8_t *p, uint16_t n) { nx_put_u16(p + 8, n); }
static void sp_set_free_off(uint8_t *p, uint16_t o) { nx_put_u16(p + 10, o); }
static uint16_t slot_off(const uint8_t *p, unsigned i) { return nx_get_u16(p + SP_HEADER + SP_SLOT * i); }
static uint16_t slot_len(const uint8_t *p, unsigned i) { return nx_get_u16(p + SP_HEADER + SP_SLOT * i + 2); }
static void slot_set(uint8_t *p, unsigned i, uint16_t off, uint16_t len)
{
    nx_put_u16(p + SP_HEADER + SP_SLOT * i, off);
    nx_put_u16(p + SP_HEADER + SP_SLOT * i + 2, len);
}

/* A fresh (zeroed) page has free_offset 0, which means "no record bytes
 * yet, records start at 4096". 0 can never be a real record offset because
 * the header lives there. */
static uint32_t free_off_of(const uint8_t *p)
{
    uint16_t f = sp_free_off(p);
    return f == 0 ? NX_PAGE_SIZE : f;
}

static uint32_t page_free_total(const uint8_t *p)
{
    unsigned n = sp_slots(p);
    uint32_t used = SP_HEADER + SP_SLOT * n;
    for (unsigned i = 0; i < n; i++)
        used += slot_len(p, i);
    return NX_PAGE_SIZE - used;
}

/* Checks a page read from disk before trusting its slot array. */
static bool page_sane(const uint8_t *p)
{
    unsigned n = sp_slots(p);
    uint32_t fo = free_off_of(p);
    if (SP_HEADER + SP_SLOT * n > fo || fo > NX_PAGE_SIZE)
        return false;
    for (unsigned i = 0; i < n; i++) {
        uint32_t o = slot_off(p, i), l = slot_len(p, i);
        if (l && (o < fo || o + l > NX_PAGE_SIZE))
            return false;
    }
    return true;
}

/* Moves all live records to the end of the page, closing holes. */
static void compact_page(uint8_t *p)
{
    uint8_t tmp[NX_PAGE_SIZE];
    memcpy(tmp, p, NX_PAGE_SIZE);
    unsigned n = sp_slots(p);
    uint32_t end = NX_PAGE_SIZE;
    for (unsigned i = 0; i < n; i++) {
        uint16_t l = slot_len(tmp, i);
        if (!l)
            continue;
        end -= l;
        memcpy(p + end, tmp + slot_off(tmp, i), l);
        slot_set(p, i, (uint16_t)end, l);
    }
    memset(p + SP_HEADER + SP_SLOT * n, 0, end - (SP_HEADER + SP_SLOT * n));
    sp_set_free_off(p, (uint16_t)end);
}

static int ensure_free_cap(table *t, uint32_t pages)
{
    if (pages <= t->free_cap)
        return 0;
    size_t cap = t->free_cap ? t->free_cap : 16;
    while (cap < pages)
        cap *= 2;
    uint16_t *f = realloc(t->free_total, cap * sizeof *f);
    if (!f)
        return -1;
    t->free_total = f;
    t->free_cap = cap;
    return 0;
}

#define RID(page, slot) (((uint64_t)(page) << 16) | (slot))
#define RID_PAGE(rid) ((uint32_t)((rid) >> 16))
#define RID_SLOT(rid) ((unsigned)((rid)&0xffffu))

/* Finds a page with room (or a new one) and pins it. Nothing is changed
 * yet, so a failure leaves the table untouched. */
static int pick_page(table *t, size_t need, uint32_t *page, nx_frame **fr)
{
    for (uint32_t pg = 0; pg < t->pages; pg++) {
        if (t->free_total[pg] >= need + SP_SLOT) {
            *page = pg;
            return nx_bufpool_pin(t->bp, t->file_id, pg, NX_PIN_READ, fr);
        }
    }
    if (ensure_free_cap(t, t->pages + 1) != 0)
        return -1;
    int rc = nx_bufpool_pin(t->bp, t->file_id, t->pages, NX_PIN_NEW, fr);
    if (rc != 0)
        return rc;
    *page = t->pages;
    t->free_total[t->pages] = NX_PAGE_SIZE - SP_HEADER;
    t->pages++;
    return 0;
}

/* Inserts into a pinned page known to have room. Returns the slot. */
static unsigned page_insert(uint8_t *p, const uint8_t *rec, uint16_t len)
{
    unsigned n = sp_slots(p), slot = n;
    for (unsigned i = 0; i < n; i++)
        if (slot_len(p, i) == 0) {
            slot = i;
            break;
        }
    uint32_t need = len + (slot == n ? SP_SLOT : 0);
    uint32_t contiguous = free_off_of(p) - (SP_HEADER + SP_SLOT * n);
    if (contiguous < need)
        compact_page(p);
    uint32_t off = free_off_of(p) - len;
    memcpy(p + off, rec, len);
    if (slot == n)
        sp_set_slots(p, (uint16_t)(n + 1));
    slot_set(p, slot, (uint16_t)off, len);
    sp_set_free_off(p, (uint16_t)off);
    return slot;
}

static int table_insert(table *t, const uint8_t *rec, uint16_t len, uint64_t lsn, uint64_t *rid)
{
    uint32_t pg;
    nx_frame *fr;
    int rc = pick_page(t, len, &pg, &fr);
    if (rc != 0)
        return rc;
    nx_frame_wrlock(fr);
    uint8_t *p = nx_frame_data(fr);
    unsigned slot = page_insert(p, rec, len);
    t->free_total[pg] = (uint16_t)page_free_total(p);
    nx_bufpool_mark_dirty(t->bp, fr, lsn);
    nx_frame_unlock(fr);
    nx_bufpool_unpin(t->bp, fr);
    *rid = RID(pg, slot);
    return 0;
}

static int table_delete(table *t, uint64_t rid, uint64_t lsn)
{
    nx_frame *fr;
    int rc = nx_bufpool_pin(t->bp, t->file_id, RID_PAGE(rid), NX_PIN_READ, &fr);
    if (rc != 0)
        return rc;
    nx_frame_wrlock(fr);
    uint8_t *p = nx_frame_data(fr);
    unsigned s = RID_SLOT(rid);
    if (s < sp_slots(p)) {
        memset(p + slot_off(p, s), 0, slot_len(p, s));
        slot_set(p, s, 0, 0);
    }
    t->free_total[RID_PAGE(rid)] = (uint16_t)page_free_total(p);
    nx_bufpool_mark_dirty(t->bp, fr, lsn);
    nx_frame_unlock(fr);
    nx_bufpool_unpin(t->bp, fr);
    return 0;
}

/* Overwrites a record of the same length in place. */
static int table_overwrite(table *t, uint64_t rid, const uint8_t *rec, uint16_t len, uint64_t lsn)
{
    nx_frame *fr;
    int rc = nx_bufpool_pin(t->bp, t->file_id, RID_PAGE(rid), NX_PIN_READ, &fr);
    if (rc != 0)
        return rc;
    nx_frame_wrlock(fr);
    uint8_t *p = nx_frame_data(fr);
    unsigned s = RID_SLOT(rid);
    if (s < sp_slots(p) && slot_len(p, s) == len)
        memcpy(p + slot_off(p, s), rec, len);
    else
        rc = -1;
    if (rc == 0)
        nx_bufpool_mark_dirty(t->bp, fr, lsn);
    nx_frame_unlock(fr);
    nx_bufpool_unpin(t->bp, fr);
    return rc;
}

typedef int (*scan_cb)(uint64_t rid, const uint8_t *rec, uint16_t len, void *ctx);

static int table_scan(table *t, scan_cb cb, void *ctx)
{
    if (ensure_free_cap(t, t->pages ? t->pages : 1) != 0)
        return -1;
    for (uint32_t pg = 0; pg < t->pages; pg++) {
        nx_frame *fr;
        int rc = nx_bufpool_pin(t->bp, t->file_id, pg, NX_PIN_READ, &fr);
        if (rc != 0)
            return rc;
        nx_frame_rdlock(fr);
        uint8_t *p = nx_frame_data(fr);
        if (!page_sane(p)) {
            nx_frame_unlock(fr);
            nx_bufpool_unpin(t->bp, fr);
            LOG_ERROR("catalog", "malformed slotted page file=%" PRIu32 " page=%" PRIu32,
                      t->file_id, pg);
            return -1;
        }
        t->free_total[pg] = (uint16_t)page_free_total(p);
        unsigned n = sp_slots(p);
        for (unsigned i = 0; i < n && rc == 0; i++)
            if (slot_len(p, i))
                rc = cb(RID(pg, i), p + slot_off(p, i), slot_len(p, i), ctx);
        nx_frame_unlock(fr);
        nx_bufpool_unpin(t->bp, fr);
        if (rc != 0)
            return rc;
    }
    return 0;
}

/* ---- catalog ------------------------------------------------------------------------ */

typedef struct {
    nx_user u;
    uint64_t rid;
} user_ent;

typedef struct {
    nx_file f;
    uint64_t rid;
} file_ent;

struct nx_catalog {
    pthread_rwlock_t lock; /* SPEC §9 #2 */
    table users, files;
    user_ent *uv;
    size_t un, ucap;
    file_ent *fv;
    size_t fn, fcap;
    nx_hmap64 file_idx; /* file id -> index into fv */
    uint64_t max_file_id;
    uint32_t max_user_id;
};

static int push_user(nx_catalog *c, const nx_user *u, uint64_t rid)
{
    if (c->un == c->ucap) {
        size_t cap = c->ucap ? c->ucap * 2 : 16;
        user_ent *v = realloc(c->uv, cap * sizeof *v);
        if (!v)
            return -1;
        c->uv = v;
        c->ucap = cap;
    }
    c->uv[c->un].u = *u;
    c->uv[c->un].rid = rid;
    c->un++;
    if (u->id > c->max_user_id)
        c->max_user_id = u->id;
    return 0;
}

static int push_file(nx_catalog *c, const nx_file *f, uint64_t rid)
{
    if (c->fn == c->fcap) {
        size_t cap = c->fcap ? c->fcap * 2 : 64;
        file_ent *v = realloc(c->fv, cap * sizeof *v);
        if (!v)
            return -1;
        c->fv = v;
        c->fcap = cap;
    }
    if (nx_hmap64_put(&c->file_idx, f->id, c->fn) != 0)
        return -1;
    c->fv[c->fn].f = *f;
    c->fv[c->fn].rid = rid;
    c->fn++;
    if (f->id > c->max_file_id)
        c->max_file_id = f->id;
    return 0;
}

static int load_user(uint64_t rid, const uint8_t *rec, uint16_t len, void *ctx)
{
    nx_user u;
    if (nx_user_decode(rec, len, &u) != 0) {
        LOG_ERROR("catalog", "undecodable user record rid=%" PRIu64, rid);
        return -1;
    }
    return push_user(ctx, &u, rid);
}

static int load_file(uint64_t rid, const uint8_t *rec, uint16_t len, void *ctx)
{
    nx_file f;
    if (nx_file_decode(rec, len, &f) != 0) {
        LOG_ERROR("catalog", "undecodable file record rid=%" PRIu64, rid);
        return -1;
    }
    return push_file(ctx, &f, rid);
}

int nx_catalog_open(nx_bufpool *bp, uint32_t users_file_id, uint32_t users_pages,
                    uint32_t files_file_id, uint32_t files_pages, nx_catalog **out)
{
    nx_catalog *c = calloc(1, sizeof *c);
    if (!c)
        return -1;
    pthread_rwlock_init(&c->lock, NULL);
    c->users = (table){bp, users_file_id, users_pages, NULL, 0};
    c->files = (table){bp, files_file_id, files_pages, NULL, 0};
    if (nx_hmap64_init(&c->file_idx, 1024) != 0 || table_scan(&c->users, load_user, c) != 0 ||
        table_scan(&c->files, load_file, c) != 0) {
        nx_catalog_close(c);
        return -1;
    }
    *out = c;
    return 0;
}

void nx_catalog_close(nx_catalog *c)
{
    if (!c)
        return;
    pthread_rwlock_destroy(&c->lock);
    free(c->users.free_total);
    free(c->files.free_total);
    free(c->uv);
    free(c->fv);
    nx_hmap64_free(&c->file_idx);
    free(c);
}

/* ---- apply ---------------------------------------------------------------------------- */

static user_ent *find_user(nx_catalog *c, uint32_t id)
{
    for (size_t i = 0; i < c->un; i++)
        if (c->uv[i].u.id == id)
            return &c->uv[i];
    return NULL;
}

int nx_catalog_put_user(nx_catalog *c, const nx_user *u, uint64_t lsn)
{
    if (strlen(u->username) > NX_USERNAME_MAX || strlen(u->display) > NX_DISPLAY_MAX)
        return -1;
    /* Fixed-size (zero-padded) records: an update never changes the length,
     * so it is one in-place overwrite of one page. */
    uint8_t rec[NX_USER_REC_MAX];
    memset(rec, 0, sizeof rec);
    nx_user_encode(u, rec);
    pthread_rwlock_wrlock(&c->lock);
    user_ent *e = find_user(c, u->id);
    int rc;
    if (!e) {
        uint64_t rid;
        rc = table_insert(&c->users, rec, NX_USER_REC_MAX, lsn, &rid);
        if (rc == 0)
            rc = push_user(c, u, rid);
    } else {
        rc = table_overwrite(&c->users, e->rid, rec, NX_USER_REC_MAX, lsn);
        if (rc == 0)
            e->u = *u;
    }
    pthread_rwlock_unlock(&c->lock);
    return rc;
}

int nx_catalog_add_file(nx_catalog *c, const nx_file *f, uint64_t lsn)
{
    uint8_t rec[NX_FILE_REC_MAX];
    if (strlen(f->name) > NX_FILENAME_MAX)
        return -1;
    uint16_t len = (uint16_t)nx_file_encode(f, rec);
    pthread_rwlock_wrlock(&c->lock);
    int rc = 1; /* already present: redo of a change the pages contain */
    if (!nx_hmap64_get(&c->file_idx, f->id, NULL)) {
        uint64_t rid;
        rc = table_insert(&c->files, rec, len, lsn, &rid);
        if (rc == 0)
            rc = push_file(c, f, rid);
    }
    pthread_rwlock_unlock(&c->lock);
    return rc;
}

int nx_catalog_set_file_flags(nx_catalog *c, uint64_t id, uint8_t flags, uint64_t lsn)
{
    pthread_rwlock_wrlock(&c->lock);
    uint64_t idx;
    int rc = 1; /* no such file */
    if (nx_hmap64_get(&c->file_idx, id, &idx)) {
        file_ent *e = &c->fv[idx];
        nx_file nf = e->f;
        nf.flags = flags;
        uint8_t rec[NX_FILE_REC_MAX];
        uint16_t len = (uint16_t)nx_file_encode(&nf, rec);
        rc = table_overwrite(&c->files, e->rid, rec, len, lsn);
        if (rc == 0)
            e->f.flags = flags;
    }
    pthread_rwlock_unlock(&c->lock);
    return rc;
}

int nx_catalog_delete_file(nx_catalog *c, uint64_t id, uint64_t lsn)
{
    pthread_rwlock_wrlock(&c->lock);
    uint64_t idx;
    int rc = 1; /* already gone */
    if (nx_hmap64_get(&c->file_idx, id, &idx)) {
        rc = table_delete(&c->files, c->fv[idx].rid, lsn);
        if (rc == 0) {
            /* Swap-remove from the vector and fix the moved entry's index. */
            nx_hmap64_remove(&c->file_idx, id);
            size_t last = c->fn - 1;
            if ((size_t)idx != last) {
                c->fv[idx] = c->fv[last];
                nx_hmap64_put(&c->file_idx, c->fv[idx].f.id, idx);
            }
            c->fn--;
        }
    }
    pthread_rwlock_unlock(&c->lock);
    return rc;
}

/* ---- queries -------------------------------------------------------------------------- */

bool nx_catalog_user_by_id(nx_catalog *c, uint32_t id, nx_user *out)
{
    pthread_rwlock_rdlock(&c->lock);
    user_ent *e = find_user(c, id);
    if (e && out)
        *out = e->u;
    pthread_rwlock_unlock(&c->lock);
    return e != NULL;
}

bool nx_catalog_user_by_name(nx_catalog *c, const char *username, nx_user *out)
{
    pthread_rwlock_rdlock(&c->lock);
    bool found = false;
    for (size_t i = 0; i < c->un; i++)
        if (strcmp(c->uv[i].u.username, username) == 0) {
            if (out)
                *out = c->uv[i].u;
            found = true;
            break;
        }
    pthread_rwlock_unlock(&c->lock);
    return found;
}

size_t nx_catalog_user_count(nx_catalog *c)
{
    pthread_rwlock_rdlock(&c->lock);
    size_t n = c->un;
    pthread_rwlock_unlock(&c->lock);
    return n;
}

int nx_catalog_users(nx_catalog *c, nx_user **out, size_t *count)
{
    pthread_rwlock_rdlock(&c->lock);
    nx_user *v = malloc((c->un ? c->un : 1) * sizeof *v);
    if (v)
        for (size_t i = 0; i < c->un; i++)
            v[i] = c->uv[i].u;
    *count = c->un;
    pthread_rwlock_unlock(&c->lock);
    *out = v;
    return v ? 0 : -1;
}

bool nx_catalog_file_by_id(nx_catalog *c, uint64_t id, nx_file *out)
{
    pthread_rwlock_rdlock(&c->lock);
    uint64_t idx;
    bool found = nx_hmap64_get(&c->file_idx, id, &idx);
    if (found && out)
        *out = c->fv[idx].f;
    pthread_rwlock_unlock(&c->lock);
    return found;
}

bool nx_catalog_latest_version(nx_catalog *c, uint32_t owner, const char *name, nx_file *out)
{
    pthread_rwlock_rdlock(&c->lock);
    const nx_file *best = NULL;
    for (size_t i = 0; i < c->fn; i++) {
        const nx_file *f = &c->fv[i].f;
        /* The newest file with this name (highest id): after a "new file"
         * upload reuses a name, later uploads continue that newest chain. */
        if (f->owner_id == owner && strcmp(f->name, name) == 0 && (!best || f->id > best->id))
            best = f;
    }
    if (best && out)
        *out = *best;
    pthread_rwlock_unlock(&c->lock);
    return best != NULL;
}

int nx_catalog_files(nx_catalog *c, nx_file_filter filter, void *ctx, nx_file **out, size_t *count)
{
    pthread_rwlock_rdlock(&c->lock);
    nx_file *v = malloc((c->fn ? c->fn : 1) * sizeof *v);
    size_t n = 0;
    if (v)
        for (size_t i = 0; i < c->fn; i++)
            if (!filter || filter(&c->fv[i].f, ctx))
                v[n++] = c->fv[i].f;
    pthread_rwlock_unlock(&c->lock);
    *out = v;
    *count = n;
    return v ? 0 : -1;
}

size_t nx_catalog_file_count(nx_catalog *c)
{
    pthread_rwlock_rdlock(&c->lock);
    size_t n = c->fn;
    pthread_rwlock_unlock(&c->lock);
    return n;
}

uint64_t nx_catalog_max_file_id(nx_catalog *c)
{
    pthread_rwlock_rdlock(&c->lock);
    uint64_t m = c->max_file_id;
    pthread_rwlock_unlock(&c->lock);
    return m;
}

uint32_t nx_catalog_max_user_id(nx_catalog *c)
{
    pthread_rwlock_rdlock(&c->lock);
    uint32_t m = c->max_user_id;
    pthread_rwlock_unlock(&c->lock);
    return m;
}

uint32_t nx_catalog_page_count(nx_catalog *c, bool files)
{
    pthread_rwlock_rdlock(&c->lock);
    uint32_t n = files ? c->files.pages : c->users.pages;
    pthread_rwlock_unlock(&c->lock);
    return n;
}
