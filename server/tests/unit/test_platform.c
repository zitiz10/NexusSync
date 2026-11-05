/* Tests for the platform layer: files, durability calls, directories,
 * time, randomness, and loopback sockets. Runs on every OS. */
#include "nx_test.h"
#include "platform/platform.h"

#include <pthread.h>

static void test_file_write_read_size_truncate(void)
{
    char dir[600], path[700];
    nx_test_tmpdir("file_rw", dir, sizeof dir);
    snprintf(path, sizeof path, "%s/a.bin", dir);

    plat_file *f = NULL;
    REQUIRE(plat_file_open(path, PLAT_O_WRITE | PLAT_O_CREATE | PLAT_O_TRUNC, &f) == 0);
    const char msg[] = "hello, blocks";
    CHECK(plat_pwrite_all(f, msg, sizeof msg, 0) == 0);
    /* Write past the end leaves a hole of zeros. */
    CHECK(plat_pwrite_all(f, "Z", 1, 100) == 0);
    uint64_t size = 0;
    CHECK(plat_file_size(f, &size) == 0);
    CHECK_EQ_U64(size, 101);
    CHECK(plat_fsync_file(f) == 0);
    CHECK(plat_fsync_dir(dir) == 0);

    char buf[128];
    memset(buf, 0x55, sizeof buf);
    CHECK(plat_pread(f, buf, sizeof msg, 0) == (int64_t)sizeof msg);
    CHECK(memcmp(buf, msg, sizeof msg) == 0);
    CHECK(plat_pread(f, buf, 1, 50) == 1);
    CHECK(buf[0] == 0);
    /* Reading across EOF returns the short count, not an error. */
    CHECK(plat_pread(f, buf, 10, 95) == 6);
    CHECK(plat_pread(f, buf, 10, 1000) == 0);

    CHECK(plat_file_truncate(f, 5) == 0);
    CHECK(plat_file_size(f, &size) == 0);
    CHECK_EQ_U64(size, 5);
    CHECK(plat_file_close(f) == 0);
}

static void test_open_flags(void)
{
    char dir[600], path[700];
    nx_test_tmpdir("open_flags", dir, sizeof dir);
    snprintf(path, sizeof path, "%s/x", dir);
    plat_file *f = NULL;

    CHECK(plat_file_open(path, PLAT_O_READ, &f) != 0);
    CHECK(plat_last_error_is_not_found());

    REQUIRE(plat_file_open(path, PLAT_O_WRITE | PLAT_O_CREATE | PLAT_O_EXCL, &f) == 0);
    CHECK(plat_pwrite_all(f, "abc", 3, 0) == 0);
    CHECK(plat_file_close(f) == 0);

    /* EXCL on an existing file fails. */
    CHECK(plat_file_open(path, PLAT_O_WRITE | PLAT_O_CREATE | PLAT_O_EXCL, &f) != 0);

    /* CREATE without TRUNC keeps contents. */
    REQUIRE(plat_file_open(path, PLAT_O_WRITE | PLAT_O_CREATE, &f) == 0);
    uint64_t size = 0;
    CHECK(plat_file_size(f, &size) == 0);
    CHECK_EQ_U64(size, 3);
    CHECK(plat_file_close(f) == 0);

    /* TRUNC empties it. */
    REQUIRE(plat_file_open(path, PLAT_O_WRITE | PLAT_O_TRUNC, &f) == 0);
    CHECK(plat_file_size(f, &size) == 0);
    CHECK_EQ_U64(size, 0);
    CHECK(plat_file_close(f) == 0);
}

static void test_rename_replace_and_remove(void)
{
    char dir[600], a[700], b[700];
    nx_test_tmpdir("rename", dir, sizeof dir);
    snprintf(a, sizeof a, "%s/a", dir);
    snprintf(b, sizeof b, "%s/b", dir);
    plat_file *f = NULL;
    REQUIRE(plat_file_open(a, PLAT_O_WRITE | PLAT_O_CREATE, &f) == 0);
    CHECK(plat_pwrite_all(f, "new", 3, 0) == 0);
    CHECK(plat_file_close(f) == 0);
    REQUIRE(plat_file_open(b, PLAT_O_WRITE | PLAT_O_CREATE, &f) == 0);
    CHECK(plat_pwrite_all(f, "old-longer", 10, 0) == 0);
    CHECK(plat_file_close(f) == 0);

    CHECK(plat_rename_replace(a, b) == 0);
    CHECK(!plat_path_exists(a));
    REQUIRE(plat_file_open(b, PLAT_O_READ, &f) == 0);
    char buf[16] = {0};
    CHECK(plat_pread(f, buf, sizeof buf, 0) == 3);
    CHECK(memcmp(buf, "new", 3) == 0);
    CHECK(plat_file_close(f) == 0);

    CHECK(plat_remove(b) == 0);
    CHECK(!plat_path_exists(b));
    CHECK(plat_remove(b) != 0);
}

static void test_delete_while_open(void)
{
    /* The engine deletes old WAL segments while readers may hold them.
     * On Windows this needs FILE_SHARE_DELETE. */
    char dir[600], path[700];
    nx_test_tmpdir("delete_open", dir, sizeof dir);
    snprintf(path, sizeof path, "%s/seg", dir);
    plat_file *f = NULL;
    REQUIRE(plat_file_open(path, PLAT_O_WRITE | PLAT_O_CREATE, &f) == 0);
    CHECK(plat_pwrite_all(f, "x", 1, 0) == 0);
    CHECK(plat_remove(path) == 0);
    char c = 0;
    CHECK(plat_pread(f, &c, 1, 0) == 1); /* still readable through the open handle */
    CHECK(c == 'x');
    CHECK(plat_file_close(f) == 0);
}

struct count_ctx {
    int files, dirs;
};

static int count_cb(const char *name, bool is_dir, void *vctx)
{
    struct count_ctx *c = vctx;
    (void)name;
    if (is_dir)
        c->dirs++;
    else
        c->files++;
    return 0;
}

static void test_dirs(void)
{
    char dir[600], deep[700], file[800];
    nx_test_tmpdir("dirs", dir, sizeof dir);
    snprintf(deep, sizeof deep, "%s/a/b/c", dir);
    CHECK(plat_mkdir_p(deep) == 0);
    CHECK(plat_mkdir_p(deep) == 0); /* idempotent */
    CHECK(plat_is_dir(deep));
    snprintf(file, sizeof file, "%s/f1", deep);
    plat_file *f = NULL;
    REQUIRE(plat_file_open(file, PLAT_O_WRITE | PLAT_O_CREATE, &f) == 0);
    CHECK(plat_file_close(f) == 0);
    snprintf(file, sizeof file, "%s/f2", deep);
    REQUIRE(plat_file_open(file, PLAT_O_WRITE | PLAT_O_CREATE, &f) == 0);
    CHECK(plat_file_close(f) == 0);
    snprintf(file, sizeof file, "%s/sub", deep);
    CHECK(plat_mkdir(file) == 0);

    struct count_ctx c = {0, 0};
    CHECK(plat_list_dir(deep, count_cb, &c) == 0);
    CHECK(c.files == 2);
    CHECK(c.dirs == 1);

    snprintf(file, sizeof file, "%s/a", dir);
    CHECK(plat_remove_tree(file) == 0);
    CHECK(!plat_path_exists(file));
    CHECK(plat_remove_tree(file) == 0); /* missing is fine */
}

static void test_utf8_paths(void)
{
    char dir[600], path[700];
    nx_test_tmpdir("utf8", dir, sizeof dir);
    /* "notes-é-日本.csv" in UTF-8: Windows must convert to UTF-16. */
    snprintf(path, sizeof path, "%s/notes-\xC3\xA9-\xE6\x97\xA5\xE6\x9C\xAC.csv", dir);
    plat_file *f = NULL;
    REQUIRE(plat_file_open(path, PLAT_O_WRITE | PLAT_O_CREATE, &f) == 0);
    CHECK(plat_file_close(f) == 0);
    CHECK(plat_path_exists(path));
    CHECK(plat_remove(path) == 0);
}

static void test_time(void)
{
    uint64_t now = plat_now_unix_s();
    CHECK(now > 1700000000u); /* after Nov 2023 */
    uint64_t m0 = plat_now_mono_ms();
    plat_sleep_ms(20);
    uint64_t m1 = plat_now_mono_ms();
    CHECK(m1 >= m0 + 15);
    char iso[32];
    plat_format_utc_iso(0, iso, sizeof iso);
    CHECK_STR(iso, "1970-01-01T00:00:00.000Z");
    plat_format_utc_iso(1762352823123ull, iso, sizeof iso);
    CHECK_STR(iso, "2025-11-05T14:27:03.123Z");
}

static void test_random(void)
{
    unsigned char a[64], b[64], zero[64];
    memset(zero, 0, sizeof zero);
    CHECK(plat_random_bytes(a, sizeof a) == 0);
    CHECK(plat_random_bytes(b, sizeof b) == 0);
    CHECK(memcmp(a, b, sizeof a) != 0);
    CHECK(memcmp(a, zero, sizeof a) != 0);
    /* Rough sanity: 64 KB of output should use every byte value. */
    static unsigned char big[65536];
    CHECK(plat_random_bytes(big, sizeof big) == 0);
    int seen[256] = {0};
    for (size_t i = 0; i < sizeof big; i++)
        seen[big[i]] = 1;
    int distinct = 0;
    for (int i = 0; i < 256; i++)
        distinct += seen[i];
    CHECK(distinct == 256);
}

struct echo_ctx {
    plat_sock listener;
    int ok;
};

static void *echo_server(void *arg)
{
    struct echo_ctx *ctx = arg;
    plat_sock c;
    char ip[64];
    if (plat_tcp_accept(ctx->listener, &c, ip, sizeof ip) != 0)
        return NULL;
    char buf[256];
    int64_t n = plat_recv(c, buf, sizeof buf);
    if (n > 0 && plat_send_all(c, buf, (size_t)n) == 0 && strcmp(ip, "127.0.0.1") == 0)
        ctx->ok = 1;
    plat_sock_close(c);
    return NULL;
}

static void test_sockets_loopback(void)
{
    REQUIRE(plat_net_init() == 0);
    struct echo_ctx ctx = {PLAT_SOCK_INVALID, 0};
    REQUIRE(plat_tcp_listen("127.0.0.1", 0, 16, &ctx.listener) == 0);
    uint16_t port = 0;
    CHECK(plat_sock_local_port(ctx.listener, &port) == 0);
    CHECK(port != 0);

    pthread_t th;
    REQUIRE(pthread_create(&th, NULL, echo_server, &ctx) == 0);

    plat_sock c;
    REQUIRE(plat_tcp_connect("127.0.0.1", port, &c) == 0);
    CHECK(plat_sock_set_timeouts(c, 2000, 2000) == 0);
    CHECK(plat_send_all(c, "ping", 4) == 0);
    char buf[16] = {0};
    int64_t n = plat_recv(c, buf, sizeof buf);
    CHECK(n == 4);
    CHECK(memcmp(buf, "ping", 4) == 0);
    /* Server closed after echo: next recv sees end of stream. */
    CHECK(plat_recv(c, buf, sizeof buf) == PLAT_RECV_CLOSED);
    plat_sock_close(c);
    pthread_join(th, NULL);
    CHECK(ctx.ok == 1);
    plat_sock_close(ctx.listener);
    plat_net_cleanup();
}

static void test_recv_timeout(void)
{
    REQUIRE(plat_net_init() == 0);
    plat_sock l, c, s;
    REQUIRE(plat_tcp_listen("127.0.0.1", 0, 4, &l) == 0);
    uint16_t port = 0;
    CHECK(plat_sock_local_port(l, &port) == 0);
    REQUIRE(plat_tcp_connect("127.0.0.1", port, &c) == 0);
    REQUIRE(plat_tcp_accept(l, &s, NULL, 0) == 0);
    CHECK(plat_sock_set_timeouts(s, 100, 100) == 0);
    char buf[4];
    uint64_t t0 = plat_now_mono_ms();
    CHECK(plat_recv(s, buf, sizeof buf) == PLAT_RECV_TIMEOUT);
    CHECK(plat_now_mono_ms() - t0 >= 80);
    plat_sock_close(s);
    plat_sock_close(c);
    plat_sock_close(l);
    plat_net_cleanup();
}

static void test_lan_addresses(void)
{
    char addrs[8][16];
    int n = plat_lan_ipv4(addrs, 8);
    CHECK(n >= 0); /* CI machines may have zero non-loopback interfaces */
    for (int i = 0; i < n; i++)
        CHECK(strcmp(addrs[i], "127.0.0.1") != 0);
}

static const nx_test_case TESTS[] = {
    NX_CASE(test_file_write_read_size_truncate),
    NX_CASE(test_open_flags),
    NX_CASE(test_rename_replace_and_remove),
    NX_CASE(test_delete_while_open),
    NX_CASE(test_dirs),
    NX_CASE(test_utf8_paths),
    NX_CASE(test_time),
    NX_CASE(test_random),
    NX_CASE(test_sockets_loopback),
    NX_CASE(test_recv_timeout),
    NX_CASE(test_lan_addresses),
};

NX_TEST_MAIN(TESTS)
