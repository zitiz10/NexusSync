/*
 * plat_common.c — helpers built only on the portable platform API,
 * so they are written once for every OS.
 */
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct tree_ctx {
    const char *base;
    int rc;
};

static int remove_entry(const char *name, bool is_dir, void *vctx)
{
    struct tree_ctx *ctx = vctx;
    size_t need = strlen(ctx->base) + 1 + strlen(name) + 1;
    char *full = malloc(need);
    if (!full) {
        ctx->rc = -1;
        return 1;
    }
    snprintf(full, need, "%s/%s", ctx->base, name);
    int rc = is_dir ? plat_remove_tree(full) : plat_remove(full);
    free(full);
    if (rc != 0) {
        ctx->rc = -1;
        return 1;
    }
    return 0;
}

int plat_remove_tree(const char *path)
{
    if (!plat_path_exists(path))
        return 0;
    if (!plat_is_dir(path))
        return plat_remove(path);
    struct tree_ctx ctx = {path, 0};
    if (plat_list_dir(path, remove_entry, &ctx) < 0 && ctx.rc == 0)
        return -1;
    if (ctx.rc != 0)
        return -1;
    return plat_rmdir(path);
}
