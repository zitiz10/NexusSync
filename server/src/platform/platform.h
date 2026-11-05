/*
 * platform.h — the only door between NexusSync and the operating system.
 *
 * Why this file exists: the server runs on Linux, macOS, and Windows
 * (docs/DECISIONS.md D-001). Each OS spells files, durability, sockets,
 * time, and randomness differently. Everything above this layer calls
 * these functions and never includes <unistd.h>, <windows.h>, etc.
 *
 * Implementations: plat_posix.c (Linux, macOS), plat_win32.c (Windows).
 *
 * Conventions:
 *   - Functions return 0 on success and -1 on failure unless stated.
 *   - After a failure, plat_last_error() describes it (per thread).
 *   - Paths are UTF-8; on Windows they are converted to UTF-16 inside.
 */
#ifndef NX_PLATFORM_H
#define NX_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- errors ------------------------------------------------------------ */

/* Human-readable description of the last failure on this thread. */
const char *plat_last_error(void);

/* True if the last failure on this thread was "file or directory not found". */
bool plat_last_error_is_not_found(void);

/* Short OS name for --version and logs: "linux", "macos", "windows". */
const char *plat_os_name(void);

/* ---- files --------------------------------------------------------------- */

typedef struct plat_file plat_file; /* opaque */

enum {
    PLAT_O_READ = 1,   /* open for reading */
    PLAT_O_WRITE = 2,  /* open for writing (implies reading is also allowed) */
    PLAT_O_CREATE = 4, /* create if missing */
    PLAT_O_TRUNC = 8,  /* truncate to 0 bytes on open */
    PLAT_O_EXCL = 16   /* with CREATE: fail if the file exists */
};

int plat_file_open(const char *path, int flags, plat_file **out);
/* Closes and frees. Returns -1 if the OS reported an error on close
 * (data may not have been written); the handle is freed either way. */
int plat_file_close(plat_file *f);

/* Reads up to len bytes at offset, looping over short reads.
 * Returns bytes read (less than len only at end of file), or -1. */
int64_t plat_pread(plat_file *f, void *buf, size_t len, uint64_t offset);

/* Writes all len bytes at offset, looping over short writes. 0 or -1. */
int plat_pwrite_all(plat_file *f, const void *buf, size_t len, uint64_t offset);

int plat_file_size(plat_file *f, uint64_t *out);
int plat_file_truncate(plat_file *f, uint64_t size);

/*
 * Make the file's data and size durable on stable storage.
 * Linux: fsync. macOS: fcntl(F_FULLFSYNC), because plain fsync on macOS
 * does not flush the drive cache. Windows: FlushFileBuffers.
 * See docs/SPEC.md §8 and docs/DECISIONS.md D-003.
 */
int plat_fsync_file(plat_file *f);

/* Make directory entries (create/rename/delete inside dir) durable.
 * POSIX: open the directory and fsync it. Windows: no-op (NTFS journals). */
int plat_fsync_dir(const char *dir);

/* ---- paths and directories ------------------------------------------------ */

/* Create one directory. Succeeds if it already exists as a directory. */
int plat_mkdir(const char *path);
/* Create a directory and any missing parents. */
int plat_mkdir_p(const char *path);
int plat_rmdir(const char *path);
int plat_remove(const char *path);
/* Atomically replace `to` with `from` (rename over an existing file). */
int plat_rename_replace(const char *from, const char *to);
bool plat_path_exists(const char *path);
bool plat_is_dir(const char *path);

/* Calls cb(name, is_dir, ctx) for each entry except "." and "..".
 * If cb returns non-zero, listing stops and that value is returned. */
typedef int (*plat_dir_cb)(const char *name, bool is_dir, void *ctx);
int plat_list_dir(const char *path, plat_dir_cb cb, void *ctx);

/* Remove a directory tree. Used by tests and demo reset only. */
int plat_remove_tree(const char *path);

/* Path separator for building paths. '/' works on Windows too, so we use
 * it everywhere; this exists only for printing native-looking paths. */
#define PLAT_PATH_SEP '/'

/* ---- time ------------------------------------------------------------------ */

uint64_t plat_now_unix_s(void);
uint64_t plat_now_unix_ms(void);
/* Monotonic clock for measuring durations; never goes backwards. */
uint64_t plat_now_mono_ms(void);
uint64_t plat_now_mono_us(void);
void plat_sleep_ms(uint32_t ms);
/* Formats unix ms as "2026-11-05T14:27:03.123Z" into buf (>= 25 bytes). */
void plat_format_utc_iso(uint64_t unix_ms, char *buf, size_t n);

/* ---- randomness ------------------------------------------------------------ */

/* Cryptographically secure random bytes from the OS. Never falls back to
 * rand(). Linux: getrandom. macOS: arc4random_buf. Windows: BCryptGenRandom. */
int plat_random_bytes(void *buf, size_t len);

/* ---- processes ------------------------------------------------------------- */

uint32_t plat_getpid(void);
/* Exit immediately without flushing stdio or running atexit handlers.
 * Used by failpoints to simulate a crash at an exact step. */
void plat_exit_now(int code);

/* Installs SIGINT/SIGTERM (POSIX) or console Ctrl+C/close (Windows)
 * handling that calls cb once. cb must only set a flag. */
int plat_on_shutdown_signal(void (*cb)(void));

/* Read a line without echo (for --create-admin). Falls back to an echoed
 * read with a warning if stdin is not a terminal. Strips the newline.
 * Returns length, or -1. */
int plat_read_password(const char *prompt, char *buf, size_t n);

/* ---- sockets --------------------------------------------------------------- */

typedef intptr_t plat_sock;
#define PLAT_SOCK_INVALID ((plat_sock)-1)

/* Must be called once before any socket use (WSAStartup on Windows;
 * ignores SIGPIPE on POSIX). */
int plat_net_init(void);
void plat_net_cleanup(void);

/* host may be "0.0.0.0" or an IPv4/hostname. */
int plat_tcp_listen(const char *host, uint16_t port, int backlog, plat_sock *out);
/* Blocks for the next connection. peer_ip may be NULL. */
int plat_tcp_accept(plat_sock listener, plat_sock *out, char *peer_ip, size_t peer_ip_len);
int plat_tcp_connect(const char *host, uint16_t port, plat_sock *out);
/* Port the socket is bound to (useful when listening on port 0 in tests). */
int plat_sock_local_port(plat_sock s, uint16_t *out);

#define PLAT_RECV_CLOSED 0
#define PLAT_RECV_ERROR (-1)
#define PLAT_RECV_TIMEOUT (-2)
/* One recv call. Returns bytes (> 0), PLAT_RECV_CLOSED, PLAT_RECV_ERROR,
 * or PLAT_RECV_TIMEOUT if the receive timeout expired. */
int64_t plat_recv(plat_sock s, void *buf, size_t len);
/* Sends everything (loops). Never raises SIGPIPE. 0 or -1. */
int plat_send_all(plat_sock s, const void *buf, size_t len);
int plat_sock_set_timeouts(plat_sock s, uint32_t recv_ms, uint32_t send_ms);
/* Shut down both directions (wakes a thread blocked in accept/recv). */
void plat_sock_shutdown(plat_sock s);
void plat_sock_close(plat_sock s);

/* Fills up to max non-loopback IPv4 addresses (dotted strings, 16 bytes
 * each) of interfaces that are up. Returns the count, or -1. */
int plat_lan_ipv4(char (*out)[16], int max);

#endif /* NX_PLATFORM_H */
