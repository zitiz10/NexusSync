# NexusSync — decisions

Each entry: the question, the options, what we chose, and why. Newest at the bottom.

## D-001 Server platforms: portable C instead of Linux-only (M1)

**Question.** The spec (v1.0) targeted Ubuntu only. The team develops on macOS and Windows laptops and wants the server to run on them too.

**Options.**
1. *Portable C with a thin platform layer.* All OS calls go through `server/src/platform/platform.h`, implemented by `plat_posix.c` (Linux, macOS) and `plat_win32.c` (Windows, MinGW-w64).
2. *Docker everywhere.* One Linux build inside a container on every machine.
3. *Rewrite the server in Go or Rust,* which are cross-platform out of the box.

**Chosen: 1.**
- It gives a native `nexusd` on every OS with nothing extra to install on the demo laptop.
- It keeps the project's purpose: an OS and DBMS engine written from scratch in C.
- The platform differences are real OS material for the viva (see D-003).

**Why not the others.**
- Option 2 needs Docker Desktop on each server machine (WSL2 on Windows). Inside the Docker Desktop VM, `fsync` goes through a virtualized disk, which weakens the durability claim the crash demo depends on. LAN port forwarding through Docker Desktop is also a demo-day risk.
- Option 3 throws away the C/OS learning goal and the spec's viva plan.

**Cost.** About 15–20% more engine work. Windows cannot be run on the development Mac, so Windows is verified by CI (`windows-latest` with MSYS2) and by the team on a real Windows laptop.

## D-002 Crypto: in-repo implementations instead of OpenSSL (M1)

**Question.** The server needs SHA-256 (blocks, files), SHA-1 (WebSocket handshake only), HMAC-SHA256 and PBKDF2 (passwords), a constant-time compare, and secure random bytes. v1.0 used OpenSSL.

**Options.**
1. *OpenSSL.* It is the standard choice on Linux. macOS does not ship its headers, so it needs Homebrew. On Windows it needs the MSYS2 package, and shipping the DLLs with `nexusd.exe` is awkward.
2. *OS crypto APIs.* CommonCrypto on macOS, BCrypt on Windows, OpenSSL on Linux. That is three code paths to test.
3. *In-repo implementations* of the hash functions, written directly from the standards (FIPS 180-4 for SHA-256 and SHA-1, RFC 2104 for HMAC, RFC 8018 for PBKDF2). Random bytes still come from the OS.

**Chosen: 3.**
- These are deterministic, well-specified algorithms with official test vectors, not new cryptographic designs.
- "Don't roll your own crypto" is about inventing schemes or getting subtle protocol details wrong. We do neither.

**Safeguards.**
- **Correctness:** unit tests check every function against NIST and RFC vectors, and cross-check SHA-256 against Python `hashlib` on thousands of random inputs of random lengths, including all lengths around the 55/56/64-byte padding boundaries.
- **Timing leaks:** password comparison uses a constant-time function (`nx_ct_equal`).
- **Randomness:** we never implement a random number generator ourselves. `plat_random_bytes` uses `getrandom` (Linux), `arc4random_buf` (macOS), or `BCryptGenRandom` (Windows). A failure is fatal.

**Trade-off.** Our SHA-256 runs slower than OpenSSL's assembly version, which uses CPU SHA instructions. Hashing throughput is part of the evaluation, so we report it honestly. A portable C SHA-256 typically manages a few hundred MB/s per core, which is more than LAN speed.

## D-003 Durability primitive per OS (M1)

`plat_fsync_file` is the single place that decides what "make it durable" means:
- **Linux:** `fsync()`.
- **macOS:** `fcntl(F_FULLFSYNC)`. A plain `fsync()` on macOS returns once the data reaches the drive, without flushing the drive's volatile cache. Apple's documentation says to use `F_FULLFSYNC` when you need durability. If it fails with `ENOTSUP` or `EINVAL` (some network or virtual filesystems), we fall back to `fsync()` and log a warning once.
- **Windows:** `FlushFileBuffers()`.

`plat_fsync_dir` makes a create, rename, or delete durable:
- **POSIX:** opens the directory and fsyncs it.
- **Windows:** does nothing. Windows cannot open a directory as a file for flushing, and NTFS journals metadata.

**Known limitation.** All three rely on the drive honouring the flush. `kill -9` tests prove process-crash safety, not power-loss safety on every drive.

## D-004 Threads: pthreads on every OS (M1)

**Options.**
1. *pthreads everywhere.* MinGW-w64 ships winpthreads.
2. *C11 `<threads.h>`.* Missing on older macOS, and it has no rwlock.
3. *A wrapper over Win32 SRWLOCK and CONDITION_VARIABLE.*

**Chosen: 1.** It is one API with rwlocks and condition variables on all three platforms, and the locking code reads the same everywhere. MSVC is not supported, because it lacks pthreads (D-005).

## D-005 Windows toolchain: MSYS2 UCRT64 (MinGW-w64 gcc), not MSVC (M1)

MSYS2 gives us `make`, gcc, winpthreads, and a POSIX-like shell, so the same Makefile and Python scripts work on Windows. MSVC would need a second build system and a pthreads shim. On MinGW, ASan and TSan are not available, so the sanitizer runs happen on Linux and macOS (CI and locally) and Windows runs the same tests without them.

## D-006 Scripts in Python instead of bash (M1)

`crash_test`, `demo_reset`, `fsck_all`, and `check` are Python, so the same script runs on Windows without WSL. A hard kill uses `subprocess.Popen.kill()`, which sends SIGKILL on POSIX and calls `TerminateProcess` on Windows. Both give the server no chance to clean up, which is what the crash test needs.

## D-007 C test framework: a tiny in-repo runner (M1)

The spec allows no extra C dependencies. `server/tests/nx_test.h` provides `TEST(name)`, `CHECK(cond)`, `CHECK_EQ_U64(a,b)`, and a registry, in about 100 lines. Each test file is its own executable, so a crash in one suite does not hide results from the others. `make test` runs them all and fails on the first failing executable.

## D-008 Web toolchain versions (M1)

v1.0 said Node 20+. On first install, the versions I had pinned (React Router 6, Vite 5, Vitest 2) showed moderate advisories in `npm audit`: an open redirect in React Router, plus issues in dev tooling, one rated critical in Vitest's `tinypool`.

**Chosen.**
- Current releases: React Router 7, Vite 8, Vitest 5, `@vitejs/plugin-react` 6, TypeScript 6.0. TypeScript 7 (the native Go port) is too new to rely on for a graded project.
- React stays at 18, as the spec says. React Router 7 supports React 18.
- `npm audit` reports 0 vulnerabilities.

**Consequence.** Vitest 5 needs Node 22.12 or newer, so the requirement moves from Node 20 to Node 22.12+. Node 20 reached end of life in April 2026, so this costs nothing. Node is only needed to build the web app, not to run the server.

**Implied packages.** `@vitejs/plugin-react` (JSX and fast refresh for Vite), `typescript`, and `@types/react` / `@types/react-dom` are not in CLAUDE.md's allowed list by name. They are required by "Vite, React 18, TypeScript" and add nothing to the shipped bundle.

## D-009 macOS SDK selection in the Makefile (M1)

On the development Mac, `xcrun` defaulted to a leftover beta `MacOSX27.0.sdk` that is newer than the installed Command Line Tools linker. Linking failed with `tapi error: unknown architecture`.

**Fix.** When only the Command Line Tools are active and `SDKROOT` is not set, the Makefile uses `CommandLineTools/SDKs/MacOSX.sdk`. The CLT installer keeps that symlink pointing at the SDK that matches its own linker. An explicit `SDKROOT` always wins. Machines with full Xcode are unaffected.

**Alternative.** Delete the stale SDK, which needs `sudo`. That is up to the machine's owner.

## D-010 Chunker fixtures: generate, do not commit (M2)

SPEC §6.4 needs 10 fixture files, about 33 MB in total. Committing them would bloat every clone permanently, because git keeps old versions forever.

**Chosen.**
- `scripts/make_fixtures.py` regenerates the data byte-identically on any OS. Random content is SHA-256 in counter mode; the text is CSV built from SplitMix64.
- Only the small reference files are committed: `chunks_expected.txt` (36 KB) and `crypto_vectors.txt`.
- The file SHA-256 in `chunks_expected.txt` catches a generator that silently changed its output.

**Alternative.** Git LFS. It adds a tool every teammate must install, so we did not use it.

## D-011 Reference implementations for crypto tests (M2)

- **Inputs** are SplitMix64 bytes. The generator is a few lines and identical in C (`tests/nx_test_util.h`) and Python (`scripts/make_fixtures.py`), so the C test can rebuild every input itself without reading data files.
- **Expected outputs** come from Python's `hashlib`, `hmac`, and `zlib`, which wrap OpenSSL and zlib. They are an implementation independent of ours.
- **Published vectors** are also checked: the NIST examples, RFC 4231, RFC 7914, and the RFC 6455 handshake example.
- **The tests can fail.** We changed one SHA-256 round constant by one bit and the suite failed, then restored it.

## D-012 Web packages added in M2

- `hash-wasm` (runtime, already allowed by CLAUDE.md): SHA-256 in the browser over plain HTTP, where `crypto.subtle` is unavailable.
- `@types/node` (dev only): types for `node:fs` in the vitest parity test, which reads fixture files. It ships nothing to the browser.
- npm 11 skipped the install script of `fsevents`, an optional macOS file watcher. We left it unapproved: Vite falls back to polling and builds normally.
