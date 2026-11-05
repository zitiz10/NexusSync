/*
 * nexusd — the NexusSync server.
 * M1: argument parsing and --version. Later milestones add --init,
 * --create-admin, --fsck, --rebuild-index, and the server loop.
 */
#include "nexus_version.h"
#include "platform/platform.h"
#include "util/log.h"

#include <stdio.h>
#include <string.h>

static void usage(FILE *out)
{
    fprintf(out,
            "usage: nexusd [--config FILE] [--init | --create-admin | --fsck | --rebuild-index]\n"
            "       nexusd --version\n");
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0) {
            printf("nexusd %s (spec %s, %s)\n", NX_VERSION, NX_SPEC_VERSION, plat_os_name());
            return 0;
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(stdout);
            return 0;
        }
    }
    usage(stderr);
    LOG_ERROR("main", "server loop not implemented yet milestone=M1");
    return 2;
}
