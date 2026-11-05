/*
 * logrec.h — WAL record types and payloads (SPEC §5), and a transaction
 * buffer that collects a transaction's records before they are logged.
 *
 * Every change to the engine's pages is described by one of these records
 * and applied by one function (engine.c: apply_record). Normal operation
 * and crash recovery use the same apply code: recovery just replays the
 * committed records with redo=true. That is why the payloads below hold
 * everything needed to redo a change, and nothing else.
 *
 * Payloads (little-endian):
 *   CHUNK_ADD      hash[32], u32 container, u32 offset, u32 len, u32 created_at
 *   REF_INC/DEC    hash[32], u32 count
 *   FILE_ADD       full files record (catalog.h, nx_file_encode)
 *   FILE_UPDATE    u64 file id, u8 flags
 *   FILE_DEL       u64 file id
 *   USER_ADD/UPD   full users record (nx_user_encode)
 *   BLOCK_FREE     hash[32]
 *   BLOCK_MOVE     hash[32], u32 new container, u32 new offset
 *   CONTAINER_DROP u32 container id
 *   CHECKPOINT     u64 checkpoint_lsn
 *   BEGIN, COMMIT  empty
 */
#ifndef NX_LOGREC_H
#define NX_LOGREC_H

#include "engine/catalog.h"
#include "engine/exthash.h"

#include <stddef.h>
#include <stdint.h>

enum nx_rec_type {
    NX_REC_BEGIN = 1,
    NX_REC_CHUNK_ADD = 2,
    NX_REC_REF_INC = 3,
    NX_REC_REF_DEC = 4,
    NX_REC_FILE_ADD = 5,
    NX_REC_FILE_UPDATE = 6,
    NX_REC_FILE_DEL = 7,
    NX_REC_USER_ADD = 8,
    NX_REC_USER_UPDATE = 9,
    NX_REC_BLOCK_FREE = 10,
    NX_REC_BLOCK_MOVE = 11,
    NX_REC_CONTAINER_DROP = 12,
    NX_REC_COMMIT = 13,
    NX_REC_CHECKPOINT = 14
};

const char *nx_rec_type_name(uint8_t type);

#define NX_CHUNK_ADD_LEN 48u
#define NX_REF_LEN 36u
#define NX_FILE_UPDATE_LEN 9u
#define NX_FILE_DEL_LEN 8u
#define NX_BLOCK_FREE_LEN 32u
#define NX_BLOCK_MOVE_LEN 40u
#define NX_CONTAINER_DROP_LEN 4u

/* One record inside a transaction buffer. */
typedef struct {
    uint8_t type;
    uint32_t len;
    size_t off; /* payload position in nx_txn.buf */
} nx_txn_rec;

/* A transaction under construction: an ordered list of records (without
 * BEGIN and COMMIT, which the logger adds). */
typedef struct {
    uint8_t *buf;
    size_t len, cap;
    nx_txn_rec *recs;
    size_t n, rcap;
} nx_txn;

void nx_txn_init(nx_txn *t);
void nx_txn_free(nx_txn *t);
int nx_txn_add(nx_txn *t, uint8_t type, const void *payload, uint32_t len);
static inline const uint8_t *nx_txn_payload(const nx_txn *t, size_t i) { return t->buf + t->recs[i].off; }

int nx_txn_chunk_add(nx_txn *t, const nx_index_entry *e);
int nx_txn_ref(nx_txn *t, uint8_t type, const uint8_t hash[32], uint32_t count);
int nx_txn_file_add(nx_txn *t, const nx_file *f);
int nx_txn_file_update(nx_txn *t, uint64_t id, uint8_t flags);
int nx_txn_file_del(nx_txn *t, uint64_t id);
int nx_txn_user(nx_txn *t, uint8_t type, const nx_user *u);
int nx_txn_block_free(nx_txn *t, const uint8_t hash[32]);
int nx_txn_block_move(nx_txn *t, const uint8_t hash[32], uint32_t container, uint32_t offset);
int nx_txn_container_drop(nx_txn *t, uint32_t container);

/* Payload decoders: 0, or -1 if the payload is malformed. */
int nx_rec_decode_chunk_add(const uint8_t *p, uint32_t len, nx_index_entry *e);
int nx_rec_decode_ref(const uint8_t *p, uint32_t len, uint8_t hash[32], uint32_t *count);
int nx_rec_decode_file_update(const uint8_t *p, uint32_t len, uint64_t *id, uint8_t *flags);
int nx_rec_decode_u64(const uint8_t *p, uint32_t len, uint64_t *v);
int nx_rec_decode_block_move(const uint8_t *p, uint32_t len, uint8_t hash[32], uint32_t *container,
                             uint32_t *offset);

#endif
