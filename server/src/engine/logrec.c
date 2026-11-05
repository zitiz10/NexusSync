/* logrec.c — WAL record payloads and the transaction buffer. See logrec.h. */
#include "engine/logrec.h"

#include "util/bytes.h"

#include <stdlib.h>
#include <string.h>

const char *nx_rec_type_name(uint8_t type)
{
    switch (type) {
    case NX_REC_BEGIN: return "BEGIN";
    case NX_REC_CHUNK_ADD: return "CHUNK_ADD";
    case NX_REC_REF_INC: return "REF_INC";
    case NX_REC_REF_DEC: return "REF_DEC";
    case NX_REC_FILE_ADD: return "FILE_ADD";
    case NX_REC_FILE_UPDATE: return "FILE_UPDATE";
    case NX_REC_FILE_DEL: return "FILE_DEL";
    case NX_REC_USER_ADD: return "USER_ADD";
    case NX_REC_USER_UPDATE: return "USER_UPDATE";
    case NX_REC_BLOCK_FREE: return "BLOCK_FREE";
    case NX_REC_BLOCK_MOVE: return "BLOCK_MOVE";
    case NX_REC_CONTAINER_DROP: return "CONTAINER_DROP";
    case NX_REC_COMMIT: return "COMMIT";
    case NX_REC_CHECKPOINT: return "CHECKPOINT";
    default: return "?";
    }
}

void nx_txn_init(nx_txn *t) { memset(t, 0, sizeof *t); }

void nx_txn_free(nx_txn *t)
{
    free(t->buf);
    free(t->recs);
    memset(t, 0, sizeof *t);
}

int nx_txn_add(nx_txn *t, uint8_t type, const void *payload, uint32_t len)
{
    if (t->n == t->rcap) {
        size_t cap = t->rcap ? t->rcap * 2 : 16;
        nx_txn_rec *r = realloc(t->recs, cap * sizeof *r);
        if (!r)
            return -1;
        t->recs = r;
        t->rcap = cap;
    }
    if (t->cap - t->len < len) {
        size_t cap = t->cap ? t->cap : 1024;
        while (cap - t->len < len)
            cap *= 2;
        uint8_t *b = realloc(t->buf, cap);
        if (!b)
            return -1;
        t->buf = b;
        t->cap = cap;
    }
    if (len)
        memcpy(t->buf + t->len, payload, len);
    t->recs[t->n++] = (nx_txn_rec){type, len, t->len};
    t->len += len;
    return 0;
}

int nx_txn_chunk_add(nx_txn *t, const nx_index_entry *e)
{
    uint8_t p[NX_CHUNK_ADD_LEN];
    memcpy(p, e->hash, 32);
    nx_put_u32(p + 32, e->container_id);
    nx_put_u32(p + 36, e->offset);
    nx_put_u32(p + 40, e->len);
    nx_put_u32(p + 44, e->created_at);
    return nx_txn_add(t, NX_REC_CHUNK_ADD, p, sizeof p);
}

int nx_txn_ref(nx_txn *t, uint8_t type, const uint8_t hash[32], uint32_t count)
{
    uint8_t p[NX_REF_LEN];
    memcpy(p, hash, 32);
    nx_put_u32(p + 32, count);
    return nx_txn_add(t, type, p, sizeof p);
}

int nx_txn_file_add(nx_txn *t, const nx_file *f)
{
    uint8_t p[NX_FILE_REC_MAX];
    size_t n = nx_file_encode(f, p);
    return nx_txn_add(t, NX_REC_FILE_ADD, p, (uint32_t)n);
}

int nx_txn_file_update(nx_txn *t, uint64_t id, uint8_t flags)
{
    uint8_t p[NX_FILE_UPDATE_LEN];
    nx_put_u64(p, id);
    p[8] = flags;
    return nx_txn_add(t, NX_REC_FILE_UPDATE, p, sizeof p);
}

int nx_txn_file_del(nx_txn *t, uint64_t id)
{
    uint8_t p[NX_FILE_DEL_LEN];
    nx_put_u64(p, id);
    return nx_txn_add(t, NX_REC_FILE_DEL, p, sizeof p);
}

int nx_txn_user(nx_txn *t, uint8_t type, const nx_user *u)
{
    uint8_t p[NX_USER_REC_MAX];
    size_t n = nx_user_encode(u, p);
    return nx_txn_add(t, type, p, (uint32_t)n);
}

int nx_txn_block_free(nx_txn *t, const uint8_t hash[32])
{
    return nx_txn_add(t, NX_REC_BLOCK_FREE, hash, NX_BLOCK_FREE_LEN);
}

int nx_txn_block_move(nx_txn *t, const uint8_t hash[32], uint32_t container, uint32_t offset)
{
    uint8_t p[NX_BLOCK_MOVE_LEN];
    memcpy(p, hash, 32);
    nx_put_u32(p + 32, container);
    nx_put_u32(p + 36, offset);
    return nx_txn_add(t, NX_REC_BLOCK_MOVE, p, sizeof p);
}

int nx_txn_container_drop(nx_txn *t, uint32_t container)
{
    uint8_t p[NX_CONTAINER_DROP_LEN];
    nx_put_u32(p, container);
    return nx_txn_add(t, NX_REC_CONTAINER_DROP, p, sizeof p);
}

int nx_rec_decode_chunk_add(const uint8_t *p, uint32_t len, nx_index_entry *e)
{
    if (len != NX_CHUNK_ADD_LEN)
        return -1;
    memcpy(e->hash, p, 32);
    e->container_id = nx_get_u32(p + 32);
    e->offset = nx_get_u32(p + 36);
    e->len = nx_get_u32(p + 40);
    e->created_at = nx_get_u32(p + 44);
    e->refcount = 0;
    return 0;
}

int nx_rec_decode_ref(const uint8_t *p, uint32_t len, uint8_t hash[32], uint32_t *count)
{
    if (len != NX_REF_LEN)
        return -1;
    memcpy(hash, p, 32);
    *count = nx_get_u32(p + 32);
    return 0;
}

int nx_rec_decode_file_update(const uint8_t *p, uint32_t len, uint64_t *id, uint8_t *flags)
{
    if (len != NX_FILE_UPDATE_LEN)
        return -1;
    *id = nx_get_u64(p);
    *flags = p[8];
    return 0;
}

int nx_rec_decode_u64(const uint8_t *p, uint32_t len, uint64_t *v)
{
    if (len != 8)
        return -1;
    *v = nx_get_u64(p);
    return 0;
}

int nx_rec_decode_block_move(const uint8_t *p, uint32_t len, uint8_t hash[32], uint32_t *container,
                             uint32_t *offset)
{
    if (len != NX_BLOCK_MOVE_LEN)
        return -1;
    memcpy(hash, p, 32);
    *container = nx_get_u32(p + 32);
    *offset = nx_get_u32(p + 36);
    return 0;
}
