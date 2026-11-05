/* chunker.c — streaming wrapper around the cut functions (see chunk.h). */
#include "engine/chunk.h"

#include <string.h>

void nx_chunker_init(nx_chunker *c, enum nx_chunker_kind kind)
{
    c->kind = kind;
    c->len = 0;
}

static size_t cut(const nx_chunker *c, size_t n)
{
    return c->kind == NX_CHUNKER_FIXED ? nx_fixed_cut(c->buf, n) : nx_cdc_cut(c->buf, n);
}

/* Emits one block from the front of the buffer and shifts the rest down. */
static int emit_one(nx_chunker *c, nx_chunk_cb cb, void *ctx)
{
    size_t n = cut(c, c->len);
    int rc = cb(c->buf, n, ctx);
    memmove(c->buf, c->buf + n, c->len - n);
    c->len -= n;
    return rc;
}

int nx_chunker_push(nx_chunker *c, const void *data, size_t len, nx_chunk_cb cb, void *ctx)
{
    const uint8_t *p = data;
    while (len > 0) {
        size_t take = sizeof c->buf - c->len;
        if (take > len)
            take = len;
        memcpy(c->buf + c->len, p, take);
        c->len += take;
        p += take;
        len -= take;
        /* Only cut with a full buffer: then the cut sees the same bytes it
         * would see if the whole file were in memory. */
        while (c->len == sizeof c->buf) {
            int rc = emit_one(c, cb, ctx);
            if (rc != 0)
                return rc;
        }
    }
    return 0;
}

int nx_chunker_finish(nx_chunker *c, nx_chunk_cb cb, void *ctx)
{
    while (c->len > 0) {
        int rc = emit_one(c, cb, ctx);
        if (rc != 0)
            return rc;
    }
    return 0;
}
