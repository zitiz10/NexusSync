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

---

## M2 — Chunking and hashing

### What it does
- **Splits a file into blocks.** `server/src/engine/chunk_cdc.c` and `chunk_fixed.c` decide where each block ends. `chunker.c` wraps them so a file can be fed in pieces of any size and still produce the same blocks.
- **Fingerprints each block with SHA-256.** `server/src/crypto/sha256.c` implements the standard (FIPS 180-4) directly. The server stores the block under its fingerprint, so a block with an identical fingerprint is never stored twice.
- **Supports passwords and the WebSocket handshake.**
  - `hmac.c` and `pbkdf2.c` turn a password into a slow-to-guess hash (used in M6).
  - `sha1.c` exists only because the WebSocket standard requires it.
  - `util/crc32.c` is a cheap checksum that detects torn or corrupted records on disk (M3–M5).
- **Matches the browser exactly.** `web/src/engine/chunker.ts` is a line-by-line copy of the C chunker. The browser chunks and fingerprints the file before uploading, and asks the server which blocks it already has.

### Why content-defined chunking
Fixed 4 KB blocks fail after an insertion. Add one character at the start of a file and every later byte moves one place, so every block boundary now falls on different content and nothing matches the old version.

Content-defined chunking (CDC) decides boundaries from the content itself:
- A **rolling hash** runs over the bytes: `h = (h << 1) + GEAR[byte]`.
- `GEAR` is a table of 256 random 32-bit numbers. Because `h` shifts left once per byte, a byte stops affecting `h` after 32 more bytes, so `h` depends only on the last 32 bytes.
- When the top bits of `h` are all zero, we cut.
- After an insertion, the boundaries after it are found at the same *content* again, so those blocks are byte-identical to before.

**Measured on our fixtures:** inserting 300 bytes into a 10 MB CSV, CDC reused **2,169 of 2,170 blocks**; fixed 4 KB blocks reused **1 of 2,561**.

**Normalised chunking.** Before 4 KB we require 14 zero bits (a cut is rare); after 4 KB only 10 (a cut comes soon). This pulls sizes towards 4 KB. Blocks are never smaller than 2 KB (tiny blocks would bloat the index) and never larger than 16 KB (a hard cap). Our measured average is about **4.9 KB**, above 4 KB because no cut is allowed before 2 KB.

### Why two implementations must agree exactly
The browser decides the blocks and their fingerprints. If its cut points differed from the server's by even one byte, a re-uploaded file would produce different fingerprints and nothing would deduplicate.
- The JavaScript code uses `>>> 0` after every step to force unsigned 32-bit arithmetic, the same as C's `uint32_t`.
- One Python script (`scripts/gen_gear.py`) writes the Gear table for both languages from one seed.
- A test feeds all 10 fixtures through both versions and compares every block length and hash with the reference file `chunks_expected.txt`, written by the C code.

### OS and DBMS concepts
- **Hashing as an index key (DBMS).** A SHA-256 fingerprint acts as the primary key of a block. Content-addressed storage like this is how Git, restic, and ZFS dedup work.
- **Streaming with bounded memory (OS).** The chunker never needs more than 16 KB buffered, so a 700 MB ISO is processed with constant memory, whether in the server or in a browser Web Worker reading 8 MB slices.
- **Checksums vs cryptographic hashes.**
  - CRC-32 is fast and catches accidental damage such as torn writes and bit flips, but anyone can forge it.
  - SHA-256 is slower but collision-resistant, so it is safe to use as a block's identity.

### Files to read
`engine/chunk.h` (parameters and API), `engine/chunk_cdc.c` (about 20 lines of logic), `engine/chunker.c`, `crypto/sha256.c`, `web/src/engine/chunker.ts`, and the tests `server/tests/unit/test_chunk.c` and `web/src/engine/chunker.test.ts`.

### Likely examiner questions

**Q1. "Two different blocks could have the same SHA-256. Then you would return the wrong data."**
For random data, a collision becomes likely only after about 2^128 blocks (the birthday bound). That is far more blocks than all storage on Earth holds, so the risk is negligible next to disk or RAM errors. No SHA-256 collision has ever been found. Git and most backup tools make the same assumption. We also re-hash every block on download and the whole file at commit, which guards against *corruption*, a different risk from collisions.

**Q2. "Why did you write your own SHA-256 instead of using OpenSSL?"**
So the server builds on Windows and macOS without installing OpenSSL (`docs/DECISIONS.md` D-002). SHA-256 is a fixed, published algorithm, not something we designed. We check it against the official NIST vectors and against Python's `hashlib` on hundreds of inputs, including every length around the 55/56/64-byte padding edges. We also changed one constant by one bit and confirmed the tests fail. The cost is speed: our C version runs a few hundred MB/s, slower than OpenSSL's assembly but faster than a LAN.

### More questions to practise
- Why is there a minimum block size? *(Without it, cuts could fall a few bytes apart. That creates millions of tiny blocks, each costing a 52-byte index entry and a lookup.)*
- What happens to deduplication for a zip or PDF that was edited? *(These formats compress their content, so a small edit changes most of the compressed bytes and few blocks match. We show this honestly in the demo.)*
- Why does PBKDF2 use 600,000 iterations? *(To make each password guess cost about a quarter-second of CPU. An attacker who steals the user table must pay that for every guess, while a real login pays it once.)*
