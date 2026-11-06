/*
 * blockhash — prints the blocks the server would store for a file: one
 * line "<length> <sha256 hex>" per block, in order, using the server's own
 * streaming chunker (SPEC §6). eval/run_eval.py uses it for the fixed-size
 * and content-defined baselines, so the evaluation measures exactly the
 * cut points the server makes, at C speed.
 *
 *   blockhash cdc|fixed <file>
 */
#include "crypto/sha256.h"
#include "engine/chunk.h"
#include "platform/platform.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define READ_SIZE (1u << 20)

static int print_block(const uint8_t *data, size_t len, void *ctx)
{
    (void)ctx;
    uint8_t h[32];
    char hex[65];
    nx_sha256(data, len, h);
    nx_hex(h, 32, hex);
    return printf("%zu %s\n", len, hex) < 0 ? -1 : 0;
}

int main(int argc, char **argv)
{
    if (argc != 3 || (strcmp(argv[1], "cdc") != 0 && strcmp(argv[1], "fixed") != 0)) {
        fprintf(stderr, "usage: blockhash cdc|fixed <file>\n");
        return 2;
    }
    plat_file *f;
    if (plat_file_open(argv[2], PLAT_O_READ, &f) != 0) {
        fprintf(stderr, "blockhash: cannot open %s: %s\n", argv[2], plat_last_error());
        return 1;
    }
    uint8_t *buf = malloc(READ_SIZE);
    if (!buf) {
        plat_file_close(f);
        return 1;
    }
    nx_chunker c;
    nx_chunker_init(&c, strcmp(argv[1], "cdc") == 0 ? NX_CHUNKER_CDC : NX_CHUNKER_FIXED);
    uint64_t off = 0;
    int rc = 0;
    for (;;) {
        int64_t got = plat_pread(f, buf, READ_SIZE, off);
        if (got < 0) {
            fprintf(stderr, "blockhash: read failed: %s\n", plat_last_error());
            rc = 1;
            break;
        }
        if (got == 0)
            break;
        if (nx_chunker_push(&c, buf, (size_t)got, print_block, NULL) != 0) {
            rc = 1;
            break;
        }
        off += (uint64_t)got;
    }
    if (rc == 0 && nx_chunker_finish(&c, print_block, NULL) != 0)
        rc = 1;
    if (fflush(stdout) != 0)
        rc = 1;
    free(buf);
    plat_file_close(f);
    return rc;
}
