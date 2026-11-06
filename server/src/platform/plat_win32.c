/*
 * plat_win32.c — platform layer for Windows (MSYS2 / MinGW-w64).
 * See platform.h for the contract and docs/DECISIONS.md D-003..D-005.
 *
 * Notes for readers coming from POSIX:
 *   - Paths are UTF-8 in our code and UTF-16 ("wide") for Win32 calls.
 *   - Files are opened with FILE_SHARE_DELETE so a WAL segment or
 *     container can be deleted while another handle has it open.
 *   - Positioned I/O uses ReadFile/WriteFile with an OVERLAPPED offset.
 *   - Durability is FlushFileBuffers. Directories need no flush (NTFS
 *     journals metadata), so plat_fsync_dir is a no-op.
 */
#ifdef _WIN32

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00 /* Windows 10 */
#endif
#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <iphlpapi.h>

#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- errors ------------------------------------------------------------ */

static _Thread_local char g_err[256];
static _Thread_local DWORD g_err_code;

static void set_err_code(const char *what, const char *detail, DWORD code)
{
    g_err_code = code;
    char sys[160] = {0};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code, 0, sys,
                   (DWORD)sizeof sys, NULL);
    size_t n = strlen(sys);
    while (n > 0 && (sys[n - 1] == '\n' || sys[n - 1] == '\r' || sys[n - 1] == ' '))
        sys[--n] = '\0';
    if (detail)
        snprintf(g_err, sizeof g_err, "%s %s: %s (%lu)", what, detail, sys, (unsigned long)code);
    else
        snprintf(g_err, sizeof g_err, "%s: %s (%lu)", what, sys, (unsigned long)code);
}

static void set_err(const char *what, const char *detail)
{
    set_err_code(what, detail, GetLastError());
}

static void set_wsa_err(const char *what, const char *detail)
{
    set_err_code(what, detail, (DWORD)WSAGetLastError());
}

static void set_err_msg(const char *msg)
{
    g_err_code = 0;
    snprintf(g_err, sizeof g_err, "%s", msg);
}

const char *plat_last_error(void) { return g_err[0] ? g_err : "no error"; }

bool plat_last_error_is_not_found(void)
{
    return g_err_code == ERROR_FILE_NOT_FOUND || g_err_code == ERROR_PATH_NOT_FOUND;
}

const char *plat_os_name(void) { return "windows"; }

/* UTF-8 -> newly allocated UTF-16. Caller frees. */
static wchar_t *widen(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    if (n <= 0) {
        set_err("utf8->utf16", s);
        return NULL;
    }
    wchar_t *w = malloc((size_t)n * sizeof(wchar_t));
    if (!w) {
        set_err_msg("utf8->utf16: out of memory");
        return NULL;
    }
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n);
    return w;
}

/* UTF-16 -> UTF-8 into buf. Returns 0 or -1. */
static int narrow(const wchar_t *w, char *buf, size_t n)
{
    int r = WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, (int)n, NULL, NULL);
    return r > 0 ? 0 : -1;
}

/* ---- files --------------------------------------------------------------- */

struct plat_file {
    HANDLE h;
};

int plat_file_open(const char *path, int flags, plat_file **out)
{
    DWORD access = GENERIC_READ;
    if (flags & PLAT_O_WRITE)
        access |= GENERIC_WRITE;

    DWORD disp;
    if ((flags & PLAT_O_CREATE) && (flags & PLAT_O_EXCL))
        disp = CREATE_NEW;
    else if ((flags & PLAT_O_CREATE) && (flags & PLAT_O_TRUNC))
        disp = CREATE_ALWAYS;
    else if (flags & PLAT_O_CREATE)
        disp = OPEN_ALWAYS;
    else if (flags & PLAT_O_TRUNC)
        disp = TRUNCATE_EXISTING;
    else
        disp = OPEN_EXISTING;

    wchar_t *wp = widen(path);
    if (!wp)
        return -1;
    HANDLE h = CreateFileW(wp, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, disp, FILE_ATTRIBUTE_NORMAL, NULL);
    free(wp);
    if (h == INVALID_HANDLE_VALUE) {
        set_err("open", path);
        return -1;
    }
    plat_file *f = malloc(sizeof *f);
    if (!f) {
        CloseHandle(h);
        set_err_msg("open: out of memory");
        return -1;
    }
    f->h = h;
    *out = f;
    return 0;
}

int plat_file_close(plat_file *f)
{
    if (!f)
        return 0;
    BOOL ok = CloseHandle(f->h);
    if (!ok)
        set_err("close", NULL);
    free(f);
    return ok ? 0 : -1;
}

int64_t plat_pread(plat_file *f, void *buf, size_t len, uint64_t offset)
{
    size_t done = 0;
    while (done < len) {
        size_t want = len - done;
        DWORD chunk = want > 0x40000000u ? 0x40000000u : (DWORD)want;
        OVERLAPPED ov;
        memset(&ov, 0, sizeof ov);
        uint64_t off = offset + done;
        ov.Offset = (DWORD)(off & 0xFFFFFFFFu);
        ov.OffsetHigh = (DWORD)(off >> 32);
        DWORD got = 0;
        if (!ReadFile(f->h, (char *)buf + done, chunk, &got, &ov)) {
            DWORD e = GetLastError();
            if (e == ERROR_HANDLE_EOF)
                break;
            set_err_code("ReadFile", NULL, e);
            return -1;
        }
        if (got == 0)
            break;
        done += got;
    }
    return (int64_t)done;
}

int plat_pwrite_all(plat_file *f, const void *buf, size_t len, uint64_t offset)
{
    size_t done = 0;
    while (done < len) {
        size_t want = len - done;
        DWORD chunk = want > 0x40000000u ? 0x40000000u : (DWORD)want;
        OVERLAPPED ov;
        memset(&ov, 0, sizeof ov);
        uint64_t off = offset + done;
        ov.Offset = (DWORD)(off & 0xFFFFFFFFu);
        ov.OffsetHigh = (DWORD)(off >> 32);
        DWORD put = 0;
        if (!WriteFile(f->h, (const char *)buf + done, chunk, &put, &ov)) {
            set_err("WriteFile", NULL);
            return -1;
        }
        if (put == 0) {
            set_err_msg("WriteFile: wrote 0 bytes");
            return -1;
        }
        done += put;
    }
    return 0;
}

int plat_file_size(plat_file *f, uint64_t *out)
{
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(f->h, &sz)) {
        set_err("GetFileSizeEx", NULL);
        return -1;
    }
    *out = (uint64_t)sz.QuadPart;
    return 0;
}

int plat_file_truncate(plat_file *f, uint64_t size)
{
    FILE_END_OF_FILE_INFO info;
    info.EndOfFile.QuadPart = (LONGLONG)size;
    if (!SetFileInformationByHandle(f->h, FileEndOfFileInfo, &info, sizeof info)) {
        set_err("SetFileInformationByHandle(EOF)", NULL);
        return -1;
    }
    return 0;
}

int plat_fsync_file(plat_file *f)
{
    if (!FlushFileBuffers(f->h)) {
        set_err("FlushFileBuffers", NULL);
        return -1;
    }
    return 0;
}

int plat_fsync_dir(const char *dir)
{
    /* NTFS journals directory changes; Windows offers no way to flush a
     * directory handle opened for normal use. Documented in SPEC §8. */
    (void)dir;
    return 0;
}

/* ---- paths and directories ------------------------------------------------ */

int plat_mkdir(const char *path)
{
    wchar_t *wp = widen(path);
    if (!wp)
        return -1;
    BOOL ok = CreateDirectoryW(wp, NULL);
    DWORD e = ok ? 0 : GetLastError();
    free(wp);
    if (ok)
        return 0;
    if (e == ERROR_ALREADY_EXISTS && plat_is_dir(path))
        return 0;
    set_err_code("mkdir", path, e);
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
    /* Skip a drive prefix like "C:/" so we do not try to create "C:". */
    size_t start = (n >= 2 && tmp[1] == ':') ? 3 : 1;
    for (size_t i = start; i < n; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char c = tmp[i];
            tmp[i] = '\0';
            if (plat_mkdir(tmp) != 0) {
                free(tmp);
                return -1;
            }
            tmp[i] = c;
        }
    }
    int rc = plat_mkdir(tmp);
    free(tmp);
    return rc;
}

int plat_rmdir(const char *path)
{
    wchar_t *wp = widen(path);
    if (!wp)
        return -1;
    BOOL ok = RemoveDirectoryW(wp);
    if (!ok)
        set_err("rmdir", path);
    free(wp);
    return ok ? 0 : -1;
}

int plat_remove(const char *path)
{
    wchar_t *wp = widen(path);
    if (!wp)
        return -1;
    BOOL ok = DeleteFileW(wp);
    if (!ok)
        set_err("delete", path);
    free(wp);
    return ok ? 0 : -1;
}

int plat_rename_replace(const char *from, const char *to)
{
    wchar_t *wf = widen(from);
    wchar_t *wt = wf ? widen(to) : NULL;
    if (!wf || !wt) {
        free(wf);
        return -1;
    }
    /* WRITE_THROUGH: do not return until the rename is on disk. */
    BOOL ok = MoveFileExW(wf, wt, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok)
        set_err("rename", from);
    free(wf);
    free(wt);
    return ok ? 0 : -1;
}

bool plat_path_exists(const char *path)
{
    wchar_t *wp = widen(path);
    if (!wp)
        return false;
    DWORD a = GetFileAttributesW(wp);
    free(wp);
    return a != INVALID_FILE_ATTRIBUTES;
}

bool plat_is_dir(const char *path)
{
    wchar_t *wp = widen(path);
    if (!wp)
        return false;
    DWORD a = GetFileAttributesW(wp);
    free(wp);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

int plat_list_dir(const char *path, plat_dir_cb cb, void *ctx)
{
    size_t n = strlen(path);
    char *pattern = malloc(n + 3);
    if (!pattern) {
        set_err_msg("list_dir: out of memory");
        return -1;
    }
    snprintf(pattern, n + 3, "%s/*", path);
    wchar_t *wp = widen(pattern);
    free(pattern);
    if (!wp)
        return -1;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wp, &fd);
    free(wp);
    if (h == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND)
            return 0; /* empty directory */
        set_err("list_dir", path);
        return -1;
    }
    int rc = 0;
    do {
        char name[MAX_PATH * 4];
        if (narrow(fd.cFileName, name, sizeof name) != 0)
            continue;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        rc = cb(name, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, ctx);
        if (rc != 0)
            break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return rc;
}

/* ---- time ------------------------------------------------------------------ */

/* 100 ns intervals between 1601-01-01 and 1970-01-01. */
#define EPOCH_DIFF_100NS 116444736000000000ULL

uint64_t plat_now_unix_ms(void)
{
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (t - EPOCH_DIFF_100NS) / 10000u;
}

/* Day by day from today, the first hour:minute after now. mktime
 * normalizes an overflowing tm_mday and works out daylight saving
 * (tm_isdst = -1). */
uint64_t plat_next_local_time(uint64_t now_s, int hour, int minute)
{
    time_t t = (time_t)now_s;
    struct tm tm;
    if (localtime_s(&tm, &t) != 0)
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

uint64_t plat_now_unix_s(void) { return plat_now_unix_ms() / 1000u; }

uint64_t plat_now_mono_us(void)
{
    static LARGE_INTEGER freq; /* constant after boot; racing writers store the same value */
    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    uint64_t ticks = (uint64_t)c.QuadPart, f = (uint64_t)freq.QuadPart;
    return (ticks / f) * 1000000u + (ticks % f) * 1000000u / f;
}

uint64_t plat_now_mono_ms(void) { return plat_now_mono_us() / 1000u; }

void plat_sleep_ms(uint32_t ms) { Sleep(ms); }

void plat_format_utc_iso(uint64_t unix_ms, char *buf, size_t n)
{
    time_t secs = (time_t)(unix_ms / 1000u);
    struct tm tm;
    gmtime_s(&tm, &secs); /* note: argument order differs from gmtime_r */
    snprintf(buf, n, "%04d-%02d-%02dT%02d:%02d:%02d.%03uZ", tm.tm_year + 1900, tm.tm_mon + 1,
             tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, (unsigned)(unix_ms % 1000u));
}

/* ---- randomness ------------------------------------------------------------ */

int plat_random_bytes(void *buf, size_t len)
{
    size_t done = 0;
    while (done < len) {
        size_t want = len - done;
        ULONG chunk = want > 0x7FFFFFFFu ? 0x7FFFFFFFu : (ULONG)want;
        NTSTATUS st = BCryptGenRandom(NULL, (PUCHAR)buf + done, chunk, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (st != 0) {
            set_err_code("BCryptGenRandom", NULL, (DWORD)st);
            return -1;
        }
        done += chunk;
    }
    return 0;
}

/* ---- processes ------------------------------------------------------------- */

uint32_t plat_getpid(void) { return (uint32_t)GetCurrentProcessId(); }

void plat_exit_now(int code)
{
    /* Like _exit: no atexit handlers, no stdio flush, no DLL cleanup. */
    TerminateProcess(GetCurrentProcess(), (UINT)code);
    _exit(code); /* not reached */
}

static void (*g_shutdown_cb)(void);

static BOOL WINAPI on_console_event(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT ||
        type == CTRL_SHUTDOWN_EVENT) {
        if (g_shutdown_cb)
            g_shutdown_cb();
        return TRUE;
    }
    return FALSE;
}

int plat_on_shutdown_signal(void (*cb)(void))
{
    g_shutdown_cb = cb;
    if (!SetConsoleCtrlHandler(on_console_event, TRUE)) {
        set_err("SetConsoleCtrlHandler", NULL);
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
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    BOOL console = GetConsoleMode(in, &mode);
    if (console)
        SetConsoleMode(in, mode & ~(DWORD)ENABLE_ECHO_INPUT);
    else
        fputs("(input will be visible: not a Windows console)\n", stderr);
    char *r = fgets(buf, (int)(n > 0x7FFFFFFF ? 0x7FFFFFFF : n), stdin);
    if (console) {
        SetConsoleMode(in, mode);
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
    WSADATA wsa;
    int rc = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (rc != 0) {
        set_err_code("WSAStartup", NULL, (DWORD)rc);
        return -1;
    }
    return 0;
}

void plat_net_cleanup(void) { WSACleanup(); }

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
        set_err_code("resolve", host, (DWORD)rc);
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
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        set_wsa_err("socket", NULL);
        return -1;
    }
    /* On Windows SO_REUSEADDR would let another process bind the same
     * port and steal connections. SO_EXCLUSIVEADDRUSE prevents that.
     * Windows allows rebinding over TIME_WAIT without SO_REUSEADDR. */
    BOOL one = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&one, sizeof one);
    if (bind(s, (struct sockaddr *)&sa, sizeof sa) == SOCKET_ERROR) {
        set_wsa_err("bind", NULL);
        closesocket(s);
        return -1;
    }
    if (listen(s, backlog) == SOCKET_ERROR) {
        set_wsa_err("listen", NULL);
        closesocket(s);
        return -1;
    }
    *out = (plat_sock)s;
    return 0;
}

int plat_tcp_accept(plat_sock listener, plat_sock *out, char *peer_ip, size_t peer_ip_len)
{
    struct sockaddr_in sa;
    int sl = (int)sizeof sa;
    SOCKET s = accept((SOCKET)listener, (struct sockaddr *)&sa, &sl);
    if (s == INVALID_SOCKET) {
        set_wsa_err("accept", NULL);
        return -1;
    }
    BOOL one = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    if (peer_ip && peer_ip_len > 0) {
        if (!inet_ntop(AF_INET, &sa.sin_addr, peer_ip, peer_ip_len))
            peer_ip[0] = '\0';
    }
    *out = (plat_sock)s;
    return 0;
}

int plat_tcp_connect(const char *host, uint16_t port, plat_sock *out)
{
    struct sockaddr_in sa;
    if (resolve_ipv4(host, port, &sa) != 0)
        return -1;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        set_wsa_err("socket", NULL);
        return -1;
    }
    if (connect(s, (struct sockaddr *)&sa, sizeof sa) == SOCKET_ERROR) {
        set_wsa_err("connect", host);
        closesocket(s);
        return -1;
    }
    BOOL one = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    *out = (plat_sock)s;
    return 0;
}

int plat_sock_local_port(plat_sock s, uint16_t *out)
{
    struct sockaddr_in sa;
    int sl = (int)sizeof sa;
    if (getsockname((SOCKET)s, (struct sockaddr *)&sa, &sl) == SOCKET_ERROR) {
        set_wsa_err("getsockname", NULL);
        return -1;
    }
    *out = ntohs(sa.sin_port);
    return 0;
}

int64_t plat_recv(plat_sock s, void *buf, size_t len)
{
    int want = len > 0x7FFFFFFF ? 0x7FFFFFFF : (int)len;
    int n = recv((SOCKET)s, (char *)buf, want, 0);
    if (n > 0)
        return (int64_t)n;
    if (n == 0)
        return PLAT_RECV_CLOSED;
    int e = WSAGetLastError();
    if (e == WSAETIMEDOUT || e == WSAEWOULDBLOCK)
        return PLAT_RECV_TIMEOUT;
    set_err_code("recv", NULL, (DWORD)e);
    return PLAT_RECV_ERROR;
}

int plat_send_all(plat_sock s, const void *buf, size_t len)
{
    size_t done = 0;
    while (done < len) {
        size_t want = len - done;
        int chunk = want > 0x7FFFFFFF ? 0x7FFFFFFF : (int)want;
        int n = send((SOCKET)s, (const char *)buf + done, chunk, 0);
        if (n == SOCKET_ERROR) {
            set_wsa_err("send", NULL);
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

int plat_sock_set_timeouts(plat_sock s, uint32_t recv_ms, uint32_t send_ms)
{
    /* Windows takes a DWORD of milliseconds, not a struct timeval. */
    DWORD rt = recv_ms, st = send_ms;
    if (setsockopt((SOCKET)s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&rt, sizeof rt) != 0 ||
        setsockopt((SOCKET)s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&st, sizeof st) != 0) {
        set_wsa_err("setsockopt timeouts", NULL);
        return -1;
    }
    return 0;
}

void plat_sock_shutdown(plat_sock s)
{
    if (s != PLAT_SOCK_INVALID)
        shutdown((SOCKET)s, SD_BOTH);
}

void plat_sock_close(plat_sock s)
{
    if (s != PLAT_SOCK_INVALID)
        closesocket((SOCKET)s);
}

int plat_lan_ipv4(char (*out)[16], int max)
{
    ULONG size = 16 * 1024;
    IP_ADAPTER_ADDRESSES *list = NULL;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3 && rc == ERROR_BUFFER_OVERFLOW; attempt++) {
        free(list);
        list = malloc(size);
        if (!list) {
            set_err_msg("lan_ipv4: out of memory");
            return -1;
        }
        rc = GetAdaptersAddresses(AF_INET,
                                  GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                      GAA_FLAG_SKIP_DNS_SERVER,
                                  NULL, list, &size);
    }
    if (rc != NO_ERROR) {
        free(list);
        set_err_code("GetAdaptersAddresses", NULL, rc);
        return -1;
    }
    int count = 0;
    for (IP_ADAPTER_ADDRESSES *a = list; a && count < max; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
            continue;
        for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u && count < max; u = u->Next) {
            struct sockaddr_in *sa = (struct sockaddr_in *)(void *)u->Address.lpSockaddr;
            if (sa->sin_family != AF_INET)
                continue;
            if (inet_ntop(AF_INET, &sa->sin_addr, out[count], 16))
                count++;
        }
    }
    free(list);
    return count;
}

#endif /* _WIN32 */
