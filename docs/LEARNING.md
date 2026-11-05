# NexusSync — learning notes for the viva

One section per milestone. Each explains what the module does, why it is built this way, which OS or DBMS idea it shows, and two questions an examiner might ask, with answers. Read the code next to these notes; file paths are given for each part.

---

## M1 — Scaffold and the platform layer

### What it does
M1 builds no storage features yet. It sets up the ground everything else stands on:

- **`server/src/platform/`** is the *platform layer*. It is the only code that talks to the operating system: opening and writing files, making data durable, directories, clocks, random numbers, and TCP sockets.
  - `platform.h` is the contract.
  - `plat_posix.c` implements it for Linux and macOS.
  - `plat_win32.c` implements it for Windows.
  - `plat_common.c` holds helpers written once on top of the contract, such as removing a directory tree.
- **`server/Makefile`** builds the server in four modes:
  - release
  - debug
  - `asan`: AddressSanitizer catches out-of-bounds reads and writes and use-after-free; UndefinedBehaviorSanitizer catches things like signed overflow.
  - `tsan`: ThreadSanitizer catches data races.

  It detects the OS and picks the right platform file and libraries.
- **`server/tests/nx_test.h`** is a ~120-line test runner. Each test file is its own program, so a crash in one does not hide the others.
- **`web/`** is the React + TypeScript app skeleton (built with Vite). For now it is only a placeholder page and one formatting helper with tests.
- **`scripts/check.py`** runs every test suite with one command on any OS.
- **`.github/workflows/ci.yml`** builds and tests on Ubuntu, macOS, and Windows for every push.

### Why it is built this way
The team uses macOS and Windows laptops, and the server must run on whichever laptop is free on demo day. The three OSes differ in exactly the places a storage engine cares about, so we isolate those differences in one small layer instead of scattering `#ifdef _WIN32` through the engine. The engine code above it reads the same on every OS, which also makes it easier to explain.

### OS concepts it demonstrates

**1. Durability: what "saved" really means.** When a program calls `write()`, the data first lands in the OS *page cache* in RAM, not on the disk. If the machine loses power, it is gone. To be sure data is on stable storage you must ask for a flush, and the call differs by OS:

| OS | Call we use | Why |
|---|---|---|
| Linux | `fsync(fd)` | Flushes the file's data and metadata, and asks the drive to flush its write cache |
| macOS | `fcntl(fd, F_FULLFSYNC)` | On macOS a plain `fsync` only sends data to the drive. The drive may still hold it in a volatile cache. `F_FULLFSYNC` forces the drive to flush. |
| Windows | `FlushFileBuffers(handle)` | The Windows equivalent of `fsync` |

The database idea of a *commit point* (M5) depends on this. A transaction counts as committed only after its log record has been flushed with the right call.

**2. Directory entries are data too.** Creating or renaming a file changes the *directory*, which is a separate object on disk. On Linux and macOS we open the directory and fsync it (`plat_fsync_dir`). On Windows this is not possible or needed, because NTFS records metadata changes in its own journal.

**3. Atomic rename.** Replacing a file in one step (for example a new superblock copy) uses `rename()` on POSIX, which atomically replaces the target. On Windows we use `MoveFileExW` with `MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH`.

**4. Positioned I/O.**
- `pread` and `pwrite` read or write at an explicit offset without moving a shared file position. Two threads can then read different parts of the same file safely.
- Windows has no `pread`. We pass the offset inside an `OVERLAPPED` structure to `ReadFile`/`WriteFile`.
- Both versions loop on *short reads and writes*: the OS may move fewer bytes than asked, and a correct program must keep going.

**5. Sockets.** TCP sockets are nearly the same API on all three, with these differences:
- Windows needs `WSAStartup` first.
- On POSIX, writing to a socket the peer has closed raises `SIGPIPE`, which would kill the server. We ignore that signal and use `MSG_NOSIGNAL` (Linux) or `SO_NOSIGPIPE` (macOS).
- On Windows, `SO_REUSEADDR` would let another program bind the same port and steal connections, so we use `SO_EXCLUSIVEADDRUSE`.

**6. Secure randomness.** Session tokens and password salts must be unpredictable. We never use `rand()`. The OS generator is `getrandom()` on Linux, `arc4random_buf()` on macOS, and `BCryptGenRandom()` on Windows.

**7. Threads.** All three use POSIX threads (pthreads). Windows gets them from MinGW's winpthreads library. One locking API everywhere keeps the lock-order rules (SPEC §9) identical across platforms.

### Files to read
`server/src/platform/platform.h` (start here), then `plat_posix.c`, then `plat_win32.c` side by side. The tests in `server/tests/unit/test_platform.c` show each function in use.

### Likely examiner questions

**Q1. "You call fsync. Is the data guaranteed to be on disk?"**
On Linux, `fsync` returns after the drive acknowledges the flush. On macOS, plain `fsync` is not enough, so we use `F_FULLFSYNC`, and on Windows `FlushFileBuffers`. Even then we depend on the drive honouring the flush. Some consumer drives with volatile caches do not. Our `kill -9` tests prove the *process* can crash at any moment without losing committed data. True power-loss safety also depends on the hardware, and we state that as a limitation.

**Q2. "Why not just use Docker or WSL to get Linux everywhere?"**
Inside Docker Desktop or WSL, `fsync` goes through a virtual disk managed by a VM, which weakens the durability guarantee our crash demo relies on. It also adds networking layers that often fail on demo day. A native build on each OS keeps the guarantees direct, and the per-OS differences are themselves something we can explain (see `docs/DECISIONS.md` D-001).

### More questions to practise
- What is a short write, and what happens if you ignore it? *(Data is silently lost: the rest of the buffer never reaches the file.)*
- Why is `fsync` on the directory needed after creating a file? *(Otherwise the file's data may be durable but its name may not, so after a crash the file may not exist.)*
- What does AddressSanitizer catch that a normal test run would not? *(Out-of-bounds and use-after-free accesses that happen not to crash, because the bytes they touch are still mapped.)*
