/*
 * nexusd — the NexusSync server.
 *
 *   nexusd --config FILE                  serve (default)
 *   nexusd --config FILE --init           create the data dir and join code
 *   nexusd --config FILE --create-admin   add an admin (asks on the terminal)
 *   nexusd --config FILE --fsck           full consistency check (I1–I3)
 *   nexusd --config FILE --rebuild-index  rebuild the block index from the
 *                                         containers and recipes (offline)
 *   nexusd --version
 * Without --config, the defaults of SPEC §3 are used.
 */
#include "api/app.h"
#include "api/auth.h"
#include "engine.h"
#include "net/server.h"
#include "nexus_version.h"
#include "platform/platform.h"
#include "util/config.h"
#include "util/log.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static void usage(FILE *out)
{
    fprintf(out, "usage: nexusd [--config FILE] [--init | --create-admin | --fsck | --rebuild-index]\n"
                 "       nexusd --version\n");
}

static void engine_config(const nx_config *cfg, nx_engine_config *ec)
{
    nx_engine_config_defaults(ec);
    ec->data_dir = cfg->data_dir;
    ec->container_size = cfg->container_size;
    ec->pool_pages = cfg->buffer_pool_pages;
    ec->verify_on_read = cfg->verify_on_read;
    ec->verify_on_commit = cfg->verify_on_commit;
}

static int open_engine(const nx_config *cfg, nx_engine **e)
{
    nx_engine_config ec;
    engine_config(cfg, &ec);
    return nx_engine_open(&ec, e);
}

static int cmd_init(const nx_config *cfg)
{
    if (nx_engine_create(cfg->data_dir) != 0)
        return 1;
    char code[NX_JOIN_CODE_LEN + 1];
    if (cfg->join_code[0]) {
        for (size_t i = 0; i < NX_JOIN_CODE_LEN; i++) {
            char ch = cfg->join_code[i];
            code[i] = ch >= 'a' && ch <= 'z' ? (char)(ch - 32) : ch;
        }
        code[NX_JOIN_CODE_LEN] = '\0';
    } else if (nx_join_code_generate(code) != 0) {
        return 1;
    }
    if (nx_join_code_write(cfg->data_dir, code) != 0) {
        LOG_ERROR("main", "cannot write the join code");
        return 1;
    }
    /* Shown once on the admin's terminal; never written to the log. */
    printf("Initialized %s\nJoin code for students: %s\n"
           "Next: nexusd --config FILE --create-admin\n",
           cfg->data_dir, code);
    return 0;
}

static int read_line(const char *prompt, char *buf, size_t n)
{
    printf("%s", prompt);
    fflush(stdout);
    if (!fgets(buf, (int)n, stdin))
        return -1;
    buf[strcspn(buf, "\r\n")] = '\0';
    return 0;
}

static int cmd_create_admin(const nx_config *cfg)
{
    char name[128], display[128], pw[NX_PASSWORD_MAX + 2], pw2[NX_PASSWORD_MAX + 2];
    nx_user u;
    memset(&u, 0, sizeof u);
    if (read_line("Admin username: ", name, sizeof name) != 0 ||
        !nx_username_normalize(name, u.username)) {
        fprintf(stderr, "Usernames are 3 to 32 letters, digits, dots, dashes, or underscores.\n");
        return 1;
    }
    if (read_line("Display name (Enter for \"Lab Admin\"): ", display, sizeof display) != 0)
        return 1;
    /* Check the length before copying: a silent snprintf truncation would
     * accept an over-long name, and gcc rejects it under -Werror. */
    const char *src = display[0] ? display : "Lab Admin";
    size_t src_len = strlen(src);
    if (src_len >= sizeof u.display) {
        fprintf(stderr, "Display names are 1 to 64 characters.\n");
        return 1;
    }
    memcpy(u.display, src, src_len + 1);
    if (!nx_display_valid(u.display)) {
        fprintf(stderr, "Display names are 1 to 64 characters.\n");
        return 1;
    }
    if (plat_read_password("Password (10+ characters): ", pw, sizeof pw) < 0 ||
        plat_read_password("Repeat password: ", pw2, sizeof pw2) < 0)
        return 1;
    int rc = 1;
    if (strcmp(pw, pw2) != 0) {
        fprintf(stderr, "The passwords do not match.\n");
    } else if (strlen(pw) < NX_PASSWORD_MIN || strlen(pw) > NX_PASSWORD_MAX) {
        fprintf(stderr, "Passwords need 10 to 256 characters.\n");
    } else {
        nx_engine *e;
        if (open_engine(cfg, &e) == 0) {
            u.role = NX_ROLE_ADMIN;
            u.status = NX_STATUS_ACTIVE;
            u.created_at = plat_now_unix_ms();
            u.last_seen = u.created_at;
            int r = nx_password_set(&u, pw, NX_PBKDF2_ITERS);
            if (r == 0)
                r = nx_engine_add_user(e, &u);
            if (r == NX_ENGINE_EXISTS)
                fprintf(stderr, "That username is taken.\n");
            else if (r == 0) {
                printf("Created admin %s (id %" PRIu32 ").\n", u.username, u.id);
                rc = 0;
            }
            if (nx_engine_close(e) != 0)
                rc = 1;
        }
    }
    memset(pw, 0, sizeof pw);
    memset(pw2, 0, sizeof pw2);
    return rc;
}

static int cmd_fsck(const nx_config *cfg)
{
    nx_engine *e;
    if (open_engine(cfg, &e) != 0)
        return 2;
    nx_fsck_report r;
    int rc = nx_engine_fsck(e, &r);
    printf("files %" PRIu64 ", chunk references %" PRIu64 ", index entries %" PRIu64 "\n",
           r.files_checked, r.chunks_checked, r.blocks_checked);
    printf("I1 %s (%" PRIu64 "), I2 %s (%" PRIu64 "), I3 %s (%" PRIu64 "), other %" PRIu64 "\n",
           r.i1_errors ? "FAIL" : "ok", r.i1_errors, r.i2_errors ? "FAIL" : "ok", r.i2_errors,
           r.i3_errors ? "FAIL" : "ok", r.i3_errors, r.other_errors);
    if (r.first_error[0])
        printf("first problem: %s\n", r.first_error);
    printf("%s\n", rc == 0 ? "fsck: clean" : rc == 1 ? "fsck: PROBLEMS FOUND" : "fsck: could not run");
    if (nx_engine_close(e) != 0 && rc == 0)
        rc = 2;
    return rc == 0 ? 0 : rc == 1 ? 1 : 2;
}

/* Offline, with the server stopped: nothing else may use the data dir. */
static int cmd_rebuild_index(const nx_config *cfg)
{
    nx_engine_config ec;
    engine_config(cfg, &ec);
    printf("Rebuilding the block index from the containers in %s ...\n", cfg->data_dir);
    fflush(stdout);
    nx_rebuild_report r;
    int rc = nx_engine_rebuild_index(&ec, &r);
    if (rc < 0) {
        printf("rebuild-index: could not run (see the errors above)\n");
        return 2;
    }
    printf("containers %" PRIu64 ", records %" PRIu64 " (damaged %" PRIu64 ", duplicate copies %" PRIu64
           ")\n",
           r.containers, r.records, r.damaged, r.duplicates);
    printf("index entries %" PRIu64 ", files counted %" PRIu64 "\n", r.entries, r.files);
    printf("previous index kept as %s\n", r.old_index);
    if (rc == 1) {
        printf("%" PRIu64 " referenced blocks were found in no container; %" PRIu64
               " files cannot be read in full (listed above)\n",
               r.missing_blocks, r.files_affected);
        printf("rebuild-index: done, WITH MISSING BLOCKS\n");
        return 1;
    }
    printf("rebuild-index: done. Run --fsck to check the result.\n");
    return 0;
}

static atomic_bool g_shutdown;
static void on_signal(void) { atomic_store(&g_shutdown, true); }

static int cmd_serve(const nx_config *cfg)
{
    if (plat_net_init() != 0)
        return 1;
    nx_app *app;
    if (nx_app_create(cfg, &app) != 0)
        return 1;
    nx_server *srv;
    if (nx_server_start(app, cfg->listen_host, cfg->listen_port, &srv) != 0) {
        nx_app_destroy(app);
        return 1;
    }
    if (strcmp(cfg->listen_host, "0.0.0.0") == 0) {
        char ips[8][16];
        int n = plat_lan_ipv4(ips, 8);
        for (int i = 0; i < n; i++)
            printf("NexusSync is reachable at http://%s:%u\n", ips[i],
                   (unsigned)nx_server_port(srv));
    } else {
        printf("NexusSync is reachable at http://%s:%u\n", cfg->listen_host,
               (unsigned)nx_server_port(srv));
    }
    fflush(stdout);
    plat_on_shutdown_signal(on_signal);
    /* Listening already: /api/health reports "recovering" meanwhile. */
    if (nx_app_open_engine(app) != 0) {
        LOG_ERROR("main", "could not open the data dir; see the errors above");
        nx_server_stop(srv);
        nx_app_destroy(app);
        return 1;
    }
    LOG_INFO("main", "ready");
    while (!atomic_load(&g_shutdown))
        plat_sleep_ms(200);
    LOG_INFO("main", "shutting down");
    atomic_store(&app->state, NX_APP_STOPPING);
    nx_server_stop(srv);
    nx_app_destroy(app);
    plat_net_cleanup();
    return 0;
}

int main(int argc, char **argv)
{
    const char *config_path = NULL, *cmd = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0) {
            printf("nexusd %s (spec %s, %s)\n", NX_VERSION, NX_SPEC_VERSION, plat_os_name());
            return 0;
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(stdout);
            return 0;
        }
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        } else if (!cmd && (strcmp(argv[i], "--init") == 0 || strcmp(argv[i], "--create-admin") == 0 ||
                            strcmp(argv[i], "--fsck") == 0 || strcmp(argv[i], "--rebuild-index") == 0)) {
            cmd = argv[i];
        } else {
            usage(stderr);
            return 2;
        }
    }
    nx_config cfg;
    nx_config_defaults(&cfg);
    if (config_path) {
        char err[256];
        if (nx_config_load(config_path, &cfg, err, sizeof err) != 0) {
            fprintf(stderr, "config: %s\n", err);
            return 2;
        }
    }
    if (!cmd)
        return cmd_serve(&cfg);
    if (strcmp(cmd, "--init") == 0)
        return cmd_init(&cfg);
    if (strcmp(cmd, "--create-admin") == 0)
        return cmd_create_admin(&cfg);
    if (strcmp(cmd, "--fsck") == 0)
        return cmd_fsck(&cfg);
    return cmd_rebuild_index(&cfg);
}
