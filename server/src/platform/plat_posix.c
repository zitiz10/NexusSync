/*
 * plat_posix.c — platform layer for Linux and macOS.
 * See platform.h for the contract and docs/DECISIONS.md D-003 for why
 * durability differs between the two.
 */
#if defined(__linux__)
#define _GNU_SOURCE /* getrandom, MSG_NOSIGNAL */
#elif defined(__APPLE__)
#define _DARWIN_C_SOURCE /* arc4random_buf, F_FULLFSYNC, SO_NOSIGPIPE */
#endif

#ifndef _WIN32

#include "platform.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/random.h>
#endif

/* ---- errors ---------------------------------------------------------- */

static _Thread_local char g_err[256];
static _Thread_local int g_errno_val;

static void set_err(const char *what, const char *detail)
{
    int e = errno;
    g_errno_val = e;
    if (detail)
        snprintf(g_err, sizeof g_err, "%s %s: %s", what, detail, strerror(e));
    else
        snprintf(g_err, sizeof g_err, "%s: %s", what, strerror(e));
    errno = e;
}

static void set_err_msg(const char *msg)
{
    g_errno_val = 0;
    snprintf(g_err, sizeof g_err, "%s", msg);
}

const char *plat_last_error(void) { return g_err[0] ? g_err : "no error"; }
bool plat_last_error_is_not_found(void) { return g_errno_val == ENOENT; }

const char *plat_os_name(void)
{
#if defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "posix";
#endif
}

/* ---- files ------------------------------------------------------------- */

struct plat_file {
    int fd;
};

int plat_file_open(const char *path, int flags, plat_file **out)
{
    int oflags = O_CLOEXEC;
    if (flags & PLAT_O_WRITE)
        oflags |= O_RDWR;
    else
        oflags |= O_RDONLY;
    if (flags & PLAT_O_CREATE)
        oflags |= O_CREAT;
    if (flags & PLAT_O_TRUNC)
        oflags |= O_TRUNC;
    if (flags & PLAT_O_EXCL)
        oflags |= O_EXCL;

    int fd;
    mode_t mode = (flags & PLAT_O_PRIVATE) ? 0600 : 0644;
    do {
        fd = open(path, oflags, mode);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        set_err("open", path);
        return -1;
    }
    if ((flags & PLAT_O_PRIVATE) && fchmod(fd, 0600) != 0) {
        set_err("fchmod", path);
        close(fd);
        return -1;
    }
    plat_file *f = malloc(sizeof *f);
    if (!f) {
        close(fd);
        set_err_msg("open: out of memory");
        return -1;
    }
    f->fd = fd;
    *out = f;
    return 0;
}

int plat_file_close(plat_file *f)
{
    if (!f)
        return 0;
    /* Do not retry close on EINTR: on Linux the fd is already released. */
    int rc = close(f->fd);
    if (rc != 0)
        set_err("close", NULL);
    free(f);
    return rc == 0 ? 0 : -1;
}

int64_t plat_pread(plat_file *f, void *buf, size_t len, uint64_t offset)
{
    size_t done = 0;
    while (done < len) {
        ssize_t n = pread(f->fd, (char *)buf + done, len - done, (off_t)(offset + done));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            set_err("pread", NULL);
            return -1;
        }
        if (n == 0)
            break; /* end of file */
        done += (size_t)n;
    }
    return (int64_t)done;
}

int plat_pwrite_all(plat_file *f, const void *buf, size_t len, uint64_t offset)
{
    size_t done = 0;
    while (done < len) {
        ssize_t n = pwrite(f->fd, (const char *)buf + done, len - done, (off_t)(offset + done));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            set_err("pwrite", NULL);
            return -1;
        }
        if (n == 0) { /* should not happen for regular files; avoid spinning */
            errno = EIO;
            set_err("pwrite", "wrote 0 bytes");
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

int plat_file_size(plat_file *f, uint64_t *out)
{
    struct stat st;
    if (fstat(f->fd, &st) != 0) {
        set_err("fstat", NULL);
        return -1;
    }
    *out = (uint64_t)st.st_size;
    return 0;
}

int plat_file_truncate(plat_file *f, uint64_t size)
{
    int rc;
    do {
        rc = ftruncate(f->fd, (off_t)size);
    } while (rc != 0 && errno == EINTR);
    if (rc != 0) {
        set_err("ftruncate", NULL);
        return -1;
    }
    return 0;
}

static int fsync_fd(int fd)
{
#if defined(__APPLE__)
    /* Plain fsync on macOS only pushes data to the drive; F_FULLFSYNC also
     * asks the drive to flush its cache. Some filesystems (network, some
     * virtual disks) refuse it; then fall back to fsync and warn once. */
    if (fcntl(fd, F_FULLFSYNC) == 0)
        return 0;
    if (errno == ENOTSUP || errno == EINVAL || errno == ENOTTY) {
        static int warned;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "warn platform F_FULLFSYNC unsupported, falling back to fsync\n");
        }
    } else {
        return -1;
    }
#endif
    int rc;
    do {
        rc = fsync(fd);
    } while (rc != 0 && errno == EINTR);
    return rc;
}

int plat_fsync_file(plat_file *f)
{
    if (fsync_fd(f->fd) != 0) {
        set_err("fsync", NULL);
        return -1;
    }
    return 0;
}

int plat_fsync_dir(const char *dir)
{
    int fd = open(dir, O_RDONLY | O_CLOEXEC
#ifdef O_DIRECTORY
                           | O_DIRECTORY
#endif
    );
    if (fd < 0) {
        set_err("open dir", dir);
        return -1;
    }
    int rc = fsync_fd(fd);
    if (rc != 0)
        set_err("fsync dir", dir);
    if (close(fd) != 0 && rc == 0) {
        set_err("close dir", dir);
        rc = -1;
    }
    return rc == 0 ? 0 : -1;
}

/* ---- paths and directories ------------------------------------------------ */

int plat_mkdir(const char *path)
{
    if (mkdir(path, 0755) == 0)
        return 0;
    if (errno == EEXIST && plat_is_dir(path))
        return 0;
    set_err("mkdir", path);
    return -1;
}

int plat_mkdir_p(const char *path)
{
    size_t n = strlen(path);
    if (n == 0 || n >= 4096) {
        set_err_msg("mkdir_p: bad path length");
        return -1;
    }
    char *tmp = malloc(n + 1);
    if (!tmp) {
        set_err_msg("mkdir_p: out of memory");
        return -1;
    }
    memcpy(tmp, path, n + 1);
    for (size_t i = 1; i < n; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            if (plat_mkdir(tmp) != 0) {
                free(tmp);
                return -1;
            }
            tmp[i] = '/';
        }
    }
    int rc = plat_mkdir(tmp);
    free(tmp);
    return rc;
}

int plat_rmdir(const char *path)
{
    if (rmdir(path) != 0) {
        set_err("rmdir", path);
        return -1;
    }
    return 0;
}

int plat_remove(const char *path)
{
    if (unlink(path) != 0) {
        set_err("unlink", path);
        return -1;
    }
    return 0;
}

int plat_rename_replace(const char *from, const char *to)
{
    /* rename(2) atomically replaces `to` on POSIX. */
    if (rename(from, to) != 0) {
        set_err("rename", from);
        return -1;
    }
    return 0;
}

bool plat_path_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

bool plat_is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int plat_list_dir(const char *path, plat_dir_cb cb, void *ctx)
{
    DIR *d = opendir(path);
    if (!d) {
        set_err("opendir", path);
        return -1;
    }
    int rc = 0;
    for (;;) {
        errno = 0;
        struct dirent *e = readdir(d);
        if (!e) {
            if (errno != 0) {
                set_err("readdir", path);
                rc = -1;
            }
            break;
        }
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        /* d_type is not reliable on every filesystem; stat to be sure. */
        size_t need = strlen(path) + 1 + strlen(e->d_name) + 1;
        char *full = malloc(need);
        if (!full) {
            set_err_msg("list_dir: out of memory");
            rc = -1;
            break;
        }
        snprintf(full, need, "%s/%s", path, e->d_name);
        bool is_dir = plat_is_dir(full);
        free(full);
        rc = cb(e->d_name, is_dir, ctx);
        if (rc != 0)
            break;
    }
    closedir(d);
    return rc;
}

/* ---- time ------------------------------------------------------------------ */

/* Day by day from today, the first hour:minute after now. mktime
 * normalizes an overflowing tm_mday and works out daylight saving
 * (tm_isdst = -1). */
uint64_t plat_next_local_time(uint64_t now_s, int hour, int minute)
{
    time_t t = (time_t)now_s;
    struct tm tm;
    if (localtime_r(&t, &tm) == NULL)
        return now_s + 86400u;
    for (int day = 0; day < 3; day++) {
        struct tm c = tm;
        c.tm_mday += day;
        c.tm_hour = hour;
        c.tm_min = minute;
        c.tm_sec = 0;
        c.tm_isdst = -1;
        time_t r = mktime(&c);
        if (r != (time_t)-1 && (uint64_t)r > now_s)
            return (uint64_t)r;
    }
    return now_s + 86400u;
}

uint64_t plat_now_unix_s(void) { return (uint64_t)time(NULL); }

uint64_t plat_now_unix_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

uint64_t plat_now_mono_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

uint64_t plat_now_mono_ms(void) { return plat_now_mono_us() / 1000u; }

void plat_sleep_ms(uint32_t ms)
{
    struct timespec ts = {(time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

void plat_format_utc_iso(uint64_t unix_ms, char *buf, size_t n)
{
    time_t secs = (time_t)(unix_ms / 1000u);
    struct tm tm;
    gmtime_r(&secs, &tm);
    snprintf(buf, n, "%04d-%02d-%02dT%02d:%02d:%02d.%03uZ", tm.tm_year + 1900, tm.tm_mon + 1,
             tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, (unsigned)(unix_ms % 1000u));
}

/* ---- randomness ------------------------------------------------------------ */

int plat_random_bytes(void *buf, size_t len)
{
#if defined(__APPLE__)
    arc4random_buf(buf, len); /* cannot fail */
    return 0;
#elif defined(__linux__)
    size_t done = 0;
    while (done < len) {
        ssize_t n = getrandom((char *)buf + done, len - done, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            set_err("getrandom", NULL);
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
#else
    arc4random_buf(buf, len);
    return 0;
#endif
}

/* ---- processes ------------------------------------------------------------- */

uint32_t plat_getpid(void) { return (uint32_t)getpid(); }

void plat_exit_now(int code) { _exit(code); }

static void (*g_shutdown_cb)(void);

static void on_signal(int sig)
{
    (void)sig;
    if (g_shutdown_cb)
        g_shutdown_cb();
}

int plat_on_shutdown_signal(void (*cb)(void))
{
    g_shutdown_cb = cb;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    /* No SA_RESTART: blocking accept() should return EINTR so the
     * accept loop notices the shutdown flag. */
    if (sigaction(SIGINT, &sa, NULL) != 0 || sigaction(SIGTERM, &sa, NULL) != 0) {
        set_err("sigaction", NULL);
        return -1;
    }
    return 0;
}

int plat_read_password(const char *prompt, char *buf, size_t n)
{
    if (n == 0)
        return -1;
    fputs(prompt, stderr);
    fflush(stderr);
    struct termios old, quiet;
    bool tty = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &old) == 0;
    if (tty) {
        quiet = old;
        quiet.c_lflag &= ~(tcflag_t)ECHO;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet);
    }
    char *r = fgets(buf, (int)(n > INT32_MAX ? INT32_MAX : n), stdin);
    if (tty) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
        fputc('\n', stderr);
    }
    if (!r) {
        set_err_msg("read_password: no input");
        return -1;
    }
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        buf[--len] = '\0';
    return (int)len;
}

/* ---- sockets --------------------------------------------------------------- */

int plat_net_init(void)
{
    /* A peer closing mid-write would otherwise kill the process. We also
     * pass MSG_NOSIGNAL / set SO_NOSIGPIPE, but ignoring is belt and braces. */
    signal(SIGPIPE, SIG_IGN);
    return 0;
}

void plat_net_cleanup(void) {}

static void sock_nosigpipe(int fd)
{
#if defined(SO_NOSIGPIPE)
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#else
    (void)fd;
#endif
}

static int resolve_ipv4(const char *host, uint16_t port, struct sockaddr_in *sa)
{
    memset(sa, 0, sizeof *sa);
    sa->sin_family = AF_INET;
    sa->sin_port = htons(port);
    if (!host || !*host || strcmp(host, "0.0.0.0") == 0) {
        sa->sin_addr.s_addr = htonl(INADDR_ANY);
        return 0;
    }
    if (inet_pton(AF_INET, host, &sa->sin_addr) == 1)
        return 0;
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(host, NULL, &hints, &res);
    if (rc != 0 || !res) {
        snprintf(g_err, sizeof g_err, "resolve %s: %s", host, gai_strerror(rc));
        g_errno_val = 0;
        return -1;
    }
    sa->sin_addr = ((struct sockaddr_in *)(void *)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return 0;
}

int plat_tcp_listen(const char *host, uint16_t port, int backlog, plat_sock *out)
{
    struct sockaddr_in sa;
    if (resolve_ipv4(host, port, &sa) != 0)
        return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        set_err("socket", NULL);
        return -1;
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    int one = 1;
    /* Allows restarting right after a crash while old connections sit in
     * TIME_WAIT; on POSIX it does not allow two live listeners. */
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        set_err("bind", NULL);
        close(fd);
        return -1;
    }
    if (listen(fd, backlog) != 0) {
        set_err("listen", NULL);
        close(fd);
        return -1;
    }
    *out = (plat_sock)fd;
    return 0;
}

int plat_tcp_accept(plat_sock listener, plat_sock *out, char *peer_ip, size_t peer_ip_len)
{
    struct sockaddr_in sa;
    socklen_t sl = sizeof sa;
    int fd = accept((int)listener, (struct sockaddr *)&sa, &sl);
    if (fd < 0) {
        set_err("accept", NULL);
        return -1;
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    sock_nosigpipe(fd);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (peer_ip && peer_ip_len > 0) {
        if (!inet_ntop(AF_INET, &sa.sin_addr, peer_ip, (socklen_t)peer_ip_len))
            peer_ip[0] = '\0';
    }
    *out = (plat_sock)fd;
    return 0;
}

int plat_tcp_connect(const char *host, uint16_t port, plat_sock *out)
{
    struct sockaddr_in sa;
    if (resolve_ipv4(host, port, &sa) != 0)
        return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        set_err("socket", NULL);
        return -1;
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    sock_nosigpipe(fd);
    int rc;
    do {
        rc = connect(fd, (struct sockaddr *)&sa, sizeof sa);
    } while (rc != 0 && errno == EINTR);
    if (rc != 0) {
        set_err("connect", host);
        close(fd);
        return -1;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    *out = (plat_sock)fd;
    return 0;
}

int plat_sock_local_port(plat_sock s, uint16_t *out)
{
    struct sockaddr_in sa;
    socklen_t sl = sizeof sa;
    if (getsockname((int)s, (struct sockaddr *)&sa, &sl) != 0) {
        set_err("getsockname", NULL);
        return -1;
    }
    *out = ntohs(sa.sin_port);
    return 0;
}

int64_t plat_recv(plat_sock s, void *buf, size_t len)
{
    for (;;) {
        ssize_t n = recv((int)s, buf, len, 0);
        if (n > 0)
            return (int64_t)n;
        if (n == 0)
            return PLAT_RECV_CLOSED;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return PLAT_RECV_TIMEOUT;
        set_err("recv", NULL);
        return PLAT_RECV_ERROR;
    }
}

int plat_send_all(plat_sock s, const void *buf, size_t len)
{
#if defined(MSG_NOSIGNAL)
    const int flags = MSG_NOSIGNAL;
#else
    const int flags = 0; /* macOS: SO_NOSIGPIPE set on the socket */
#endif
    size_t done = 0;
    while (done < len) {
        ssize_t n = send((int)s, (const char *)buf + done, len - done, flags);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            set_err("send", NULL);
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

int plat_sock_set_timeouts(plat_sock s, uint32_t recv_ms, uint32_t send_ms)
{
    struct timeval rt = {(time_t)(recv_ms / 1000u), (int)((recv_ms % 1000u) * 1000u)};
    struct timeval st = {(time_t)(send_ms / 1000u), (int)((send_ms % 1000u) * 1000u)};
    if (setsockopt((int)s, SOL_SOCKET, SO_RCVTIMEO, &rt, sizeof rt) != 0 ||
        setsockopt((int)s, SOL_SOCKET, SO_SNDTIMEO, &st, sizeof st) != 0) {
        set_err("setsockopt timeouts", NULL);
        return -1;
    }
    return 0;
}

void plat_sock_shutdown(plat_sock s)
{
    if (s != PLAT_SOCK_INVALID)
        shutdown((int)s, SHUT_RDWR);
}

void plat_sock_close(plat_sock s)
{
    if (s != PLAT_SOCK_INVALID)
        close((int)s);
}

int plat_lan_ipv4(char (*out)[16], int max)
{
    struct ifaddrs *ifs = NULL;
    if (getifaddrs(&ifs) != 0) {
        set_err("getifaddrs", NULL);
        return -1;
    }
    int count = 0;
    for (struct ifaddrs *i = ifs; i && count < max; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET)
            continue;
        if (!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK))
            continue;
        struct sockaddr_in *sa = (struct sockaddr_in *)(void *)i->ifa_addr;
        if (inet_ntop(AF_INET, &sa->sin_addr, out[count], 16))
            count++;
    }
    freeifaddrs(ifs);
    return count;
}

#endif /* !_WIN32 */
