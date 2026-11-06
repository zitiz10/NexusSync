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

---

## M3 — Containers, buffer pool, and the block index

### What it does
- **Containers** (`engine/container.c`) are where block bytes live: append-only files of about 4 MB.
  - Each record is the block's SHA-256, its length, a CRC-32, and the data.
  - The newest container takes appends. When the next record would not fit, the container is *sealed* (fsynced and never written again) and a new one starts.
- **The buffer pool** (`engine/bufpool.c`) is a fixed set of 4 KB frames in RAM that caches pages of the index and catalog files. Every read or change of a page goes through it.
- **The block index** (`engine/exthash.c`) maps a block's SHA-256 to where its bytes are (container, offset, length) and how many file references it has (refcount). It is built with *extendible hashing* over 4 KB bucket pages, each holding up to 78 entries.

### Extendible hashing in one picture
```
directory (global depth 2)        bucket pages
  00 ───────────────────────────► page 0 (local depth 1): hashes starting 0…
  01 ──────────────────────────┘
  10 ───────────────────────────► page 1 (local depth 2): hashes starting 10…
  11 ───────────────────────────► page 2 (local depth 2): hashes starting 11…
```
- **Lookup:** take the top *global depth* bits of the hash, read that directory slot (in memory), and read one bucket page. One page read, however big the index is.
- **Insert into a full bucket:**
  - If its *local depth* is below the global depth, several slots share the page. Split it into two pages by the next bit and repoint half the slots. The directory does not grow.
  - If local depth equals global depth, *double* the directory first (each slot is copied twice), then split.
- **Buckets never merge.** This keeps the code simple; deleted entries just leave room.

**Measured** (ASan build): 1M random hashes → global depth 15, 16,641 buckets, 77% average fill, every entry found again, also after reopening from disk. 600 hashes that share their top 16 bits force splits down to depth 20 and are all still found.

### The buffer pool
- **Pin and unpin.** Code *pins* a page while using it, so the page cannot be evicted, and unpins it when done.
- **Latch.** A per-frame read/write lock protects the bytes while they are read or changed. Pinning and latching are separate: many threads can have a page pinned, but only one can latch it for writing.
- **Clock replacement.** A "hand" sweeps the frames looking for one to reuse.
  - A frame used since the hand last passed has its reference bit set. The hand clears the bit and moves on (a *second chance*).
  - A frame with the bit clear is evicted.

  This approximates LRU without keeping a sorted list. Our test checks the exact eviction order.
- **No-steal.** A dirty (changed) page is never written to disk by eviction, only by a checkpoint. So the files on disk always show the state as of the last checkpoint, and recovery never has to undo anything. When every frame is dirty or pinned, the pool says `NX_BP_FULL` and the caller runs a checkpoint.
- **Checksums.** Every page carries a CRC-32, set when it is written and checked when it is read. A flipped bit on disk is reported, not used.

### Page LSNs (needed for recovery in M5)
Every page records the LSN (log sequence number) of the last logged change applied to it. During recovery we replay the write-ahead log. A change is skipped if the page's LSN shows it is already there. This makes replay safe to repeat, and lets a checkpoint happen partway through applying a large transaction (`docs/DECISIONS.md` D-013).

### Crash handling in containers
- A crash in the middle of an append leaves a partial record at the end of the newest container. On restart we detect it: the length runs past the end of the file, or the CRC is wrong on the last record.
- We seal that container as it is and continue in a new one, so valid records never sit after garbage.
- Nothing that was committed can be lost this way, because a block counts as stored only after the container is fsynced and the WAL says so (M5).

### OS and DBMS concepts
- **Paging and page replacement (OS):** the buffer pool *is* a page cache with Clock replacement, the same algorithm family operating systems use for virtual memory.
- **Indexing (DBMS):** extendible hashing is a dynamic hash index. Lookups take O(1) page reads, and it grows incrementally with no full rehash.
- **Buffer management policies (DBMS):** no-steal / no-force, pin counts, dirty pages.
- **Append-only storage (OS):** sequential writes are fast, and a torn write can only damage the end of a file.
- **Concurrency (OS):**
  - The directory lock is shared for normal operations and exclusive for a split.
  - Frame latches protect the bytes of one page.
  - Small *leaf* locks (pool page table, handle cache) are never held while acquiring another lock, so they cannot be part of a deadlock cycle.

### Files to read
`engine/exthash.h` (layout and rules), then `exthash.c` (`nx_exthash_split` is the heart); `engine/bufpool.c` (`find_victim` is Clock); `engine/container.c` (`nx_containers_open` handles crash leftovers). Tests: `tests/unit/test_exthash.c`, `test_bufpool.c`, `test_container.c`.

### Likely examiner questions

**Q1. "Why extendible hashing and not a B+ tree?"**
We only ever look blocks up by exact hash, never by range, so we do not need a B+ tree's ordering. Extendible hashing gives one page read per lookup and grows smoothly: a split touches one bucket and maybe doubles the small in-memory directory. A B+ tree needs about log(n) page reads and more complex splits. The cost is that range scans are impossible, which we never need. (A B+ tree over *file names* is listed as a stretch goal.)

**Q2. "What happens when the buffer pool is full of dirty pages?"**
No-steal forbids writing them out early, so `pin` returns `NX_BP_FULL` and the caller runs a checkpoint: write all dirty pages safely, then mark them clean. We also checkpoint early when more than half the frames are dirty. The test with a 64-frame pool ran 1,708 checkpoints during 200,000 operations, and the final state matched an in-memory model exactly.

### More questions to practise
- What is the difference between a pin and a latch? *(A pin says "do not evict this frame". A latch says "do not touch these bytes while I am reading or writing them".)*
- Why does doubling the directory not move any entries? *(Each old slot becomes two slots pointing at the same page. Pages split only when they are full.)*
- Why do both pages keep the *old* page's LSN after a split? *(A split only moves entries; it is not a logged change. The new page already contains every change the old page had, so redo must not apply those again. And neither page may take the LSN of the record that caused the split, or redo would skip that record when it is retried. That exact bug was in the M3 code and was found in M4: DECISIONS D-018.)*

## M4 — Catalog, recipes, and the engine API

### What it does
- **The catalog** (`engine/catalog.c`) stores users and files in two tables of 4 KB *slotted pages*, read and written through the same buffer pool as the index. At startup every record is decoded into memory, so queries never touch disk.
- **Recipes** (`engine/recipe.c`) are the "how to rebuild this file" lists: for each block in order, its SHA-256 and length. They are appended to `recipes.dat` and fsynced before the file is committed.
- **The engine** (`engine/engine.c`, API in `include/engine.h`) puts containers, index, catalog, and recipes together into *transactions*. It is the only door to storage: store blocks, commit a file, read a file, delete a file, add a user, checkpoint, fsck, stats.
- **Checkpoints** write every dirty page through a *doublewrite* file (`engine/checkpoint.c`), then a new *superblock* copy (`engine/superblock.c`).

### A slotted page
```
 0        16                                                     4096
 | header | slot 0 | slot 1 | slot 2 | → free space ←  | rec 2 | rec 0 | rec 1 |
            (offset,length) pairs grow right            records grow left
```
- A record is found by (page, slot). The slot array says where its bytes are, so records can move inside the page (compaction) without anything outside noticing.
- Deleting sets the slot's length to 0. The hole is reused later; when contiguous space runs out, the page is compacted (live records pushed to the end).
- User records are padded to a fixed size, so updating a user is always an overwrite in place.

### How a file is committed
1. The client has already sent the missing blocks (`nx_engine_store_blocks`): each block is re-hashed (the client is never trusted), appended to the active container, the container is fsynced, then one `CHUNK_ADD` record per new block is logged with refcount 0.
2. `nx_engine_commit_file` checks the chunk list adds up to the file size, checks every chunk exists, and (with `verify_on_commit`) reads the whole file back and compares its SHA-256.
3. The recipe is appended and fsynced.
4. Under the transaction mutex: check again that every chunk exists, then log `REF_INC` (one per *distinct* block, with a count) and `FILE_ADD`, and apply them to the pages.

Delete is the mirror: `FILE_DEL` plus `REF_DEC` per distinct block. A block appearing twice in one file is counted twice (invariant I2).

### One apply function for everything
Every change is a log record (`engine/logrec.h`), and `apply_record` in `engine.c` is the only code that changes pages. Normal operation calls it right after logging; recovery (M5) will call it again for records found in the WAL. So the code that runs at 3 a.m. after a crash is the same code that runs all day. In M4 the "log" only hands out LSNs; M5 adds the WAL file and its fsync at the marked point.

Applying is safe to repeat:
- catalog changes are *idempotent by key* (adding file 42 again does nothing; deleting a missing file does nothing);
- index changes are skipped when the page's LSN shows they are already there (D-013).

### fsck: checking the invariants
`nx_engine_fsck` recomputes everything from the recipes and compares:
- **I1:** every referenced block is in the index, and its bytes hash to its key;
- **I2:** every index entry's refcount equals the number of references in all recipes;
- **I3:** every file, rebuilt block by block, matches its stored SHA-256.

The tests damage each one on purpose (flip a container byte, bump a refcount, commit a file with the wrong hash) and check fsck names the problem.

### Measured (ASan build, macOS)
- 200 random CSV-like files (up to 300 KB): 30 exact duplicates sent **0 bytes**; 70 edited copies (a few bytes inserted, deleted, or overwritten) sent **11%** of their bytes; 15 new versions.
- With a 48-frame pool, the run needed 125 checkpoints, many in the middle of applying a transaction, and fsck stayed clean before deletes, after deletes, after reopening, and after deleting everything (all refcounts back to 0).
- 6 threads uploading overlapping files at once through a 16-frame pool: 40+ checkpoints, ThreadSanitizer clean, fsck clean.

### OS and DBMS concepts
- **Slotted pages (DBMS):** the standard layout for variable-length records.
- **Transactions and the commit point (DBMS):** what is durable when, and why the recipe and container bytes must be fsynced *before* the log says the file exists.
- **Redo-only logging with idempotent apply (DBMS):** a change can be replayed any number of times safely.
- **Doublewrite (DBMS, used by InnoDB):** protects against torn pages, a page half-written when power fails.
- **A/B superblocks (OS file systems):** two copies written alternately, so one is always valid.
- **Mutual exclusion and lock ordering (OS):** one transaction mutex keeps applies in LSN order; readers avoid it (D-016).

### Files to read
`include/engine.h` (the API), then `engine/engine.c` in this order: `nx_engine_commit_file`, `log_and_apply`, `apply_record`, `checkpoint_locked`, `nx_engine_fsck`. Then `engine/catalog.c` (`page_insert`, `compact_page`). Tests: `tests/unit/test_engine.c` (the 200-file round trip), `test_catalog.c`, `test_persist.c`.

### Likely examiner questions

**Q1. "What stops two students uploading the same file at the same time from corrupting the refcounts?"**
Both may store the same new block, but the index records it once: the second `CHUNK_ADD` finds the entry and does nothing, and that student's copy is just unused bytes in a container (compaction reclaims them). The refcount changes happen only at commit, under the transaction mutex, one transaction at a time, so each commit adds its own references exactly once. The concurrency test runs 6 threads with overlapping files and then checks every refcount with fsck.

**Q2. "Why write the recipe before the commit record, and what if the server crashes in between?"**
The commit record points at the recipe by offset. If the recipe were written after, a crash could leave a committed file whose recipe does not exist: the file would be lost. Written before, a crash in between leaves a recipe nothing points at. It wastes a few bytes but is harmless, because recipes are only reached through committed file records.

### More questions to practise
- Why are user records padded to a fixed size? *(An update is then an overwrite in place on one page. A longer record might not fit, and moving it would need extra logic during redo.)*
- Why check that chunks exist twice during a commit? *(The first check, without the lock, gives a fast 409 before anything is written. The second, under the transaction mutex, is the one that counts: cleanup also runs under that mutex, so no block can disappear between the check and the `REF_INC`.)*
- What does the doublewrite file protect against that the WAL does not? *(A torn page. Redo needs a readable page to apply changes to; if a crash leaves a page half old, half new, its CRC fails and there is nothing to redo onto. The complete copy in the doublewrite file restores it.)*

## M5 — Write-ahead log, checkpoints, and crash recovery

### What it does
- **The write-ahead log** (`engine/wal.c`) records every change before any page changes. A transaction is written as `BEGIN`, its records, `COMMIT`, in one write, then fsynced. **That fsync is the commit point:** once it returns, the change survives a crash.
- **Checkpoints** (`checkpoint_locked` in `engine/engine.c`) copy dirty pages to disk safely (doublewrite first), write a new superblock saying "everything up to LSN L is in the pages", then delete log segments that only hold records up to L.
- **Recovery** (`replay_log` in `engine.c`) runs on every start:
  1. load the newest valid superblock;
  2. if `doublewrite.bin` holds a complete batch, copy it in place (repairs torn pages);
  3. read the log after L, cut off a torn or uncommitted tail;
  4. redo every committed transaction through the normal `apply_record`, with `redo = true`;
  5. checkpoint, recount statistics.

### The rule that makes it work: redo-only + no-steal
- *No-steal*: a changed page never reaches disk before a checkpoint. So the pages on disk always show the state as of the last checkpoint, and never contain anything uncommitted.
- *Redo-only log*: after a crash we only need to **redo** committed work that came after the checkpoint. There is no undo, because uncommitted work never touched the pages.
- *Idempotent redo*: replaying a record that is already in a page does nothing (page LSN for the index, "by key" for the catalog). So it does not matter exactly where the crash happened.

### Timeline of one upload, and what a crash at each point leaves
| Crash after… | After restart |
|---|---|
| half a block written to a container | file absent; orphan bytes for compaction |
| blocks fsynced, `CHUNK_ADD` not logged | file absent; orphan bytes |
| `CHUNK_ADD` committed | blocks indexed with refcount 0 (cleanup frees them after the grace period) |
| recipe written, commit not logged | file absent; dead recipe bytes |
| commit record written (even before fsync, for a process crash) | **file present**, redone from the log |
| commit fsynced, pages half updated | **file present**; redo skips what is there |

`tests/unit/test_recovery.c` crashes at each of these points (and at every step of a checkpoint) and checks the exact outcome plus fsck.

### Torn pages and the doublewrite file
A 4 KB page write is not atomic on most drives: power loss can leave half old, half new. The page's CRC then fails, and **redo cannot help**, because redo needs a readable page to apply changes to. So a checkpoint first writes all pages to `doublewrite.bin` and fsyncs it. If a page is torn later, the complete copy is restored at startup. The test `test_torn_page_without_doublewrite` deletes that file after a torn checkpoint: the store then refuses to open ("page checksum mismatch") instead of serving damaged data.

### How we crash on purpose
Named *crash points* (`util/failpoint.h`, DECISIONS D-020). A test arms, say, `txn.after_commit`. When the engine reaches it, that write and every later one is skipped, and the test abandons the engine with no checkpoint, like `kill -9`. Reopening runs recovery. "Torn" points first write half of the buffer. The crash loop does this 20 times at random points during random uploads, deletes, user changes, and checkpoints, and checks the recovered store against a model of what was committed.

### Measured (ASan build, macOS)
- Cutting the log at each of 294 byte positions inside the last transaction: recovery always returns to the previous commit, and new appends continue cleanly.
- 19 crash-between-steps cases: every outcome as expected, fsck clean, the store keeps working.
- Torn superblock: recovery falls back to the older copy and replays 8 transactions on top of pages that already contain them, and fsck is clean. This shows page-LSN redo working.
- Crash loop: 20 kills over 315 operations, 14 at a crash point inside an operation; 102 transactions redone, 17 pages repaired; the state matched the model every time.

### OS and DBMS concepts
- **Write-ahead logging, commit point, durability (DBMS):** the "D" in ACID.
- **ARIES-style redo with page LSNs (DBMS):** idempotent recovery.
- **No-steal / no-force buffer policy (DBMS):** why there is no undo log.
- **Checkpointing and log truncation (DBMS):** recovery time stays short, and the log does not grow forever.
- **fsync and its OS differences (OS):** `F_FULLFSYNC` on macOS, `FlushFileBuffers` on Windows (D-003).
- **Atomicity of writes, torn writes (OS):** why CRCs and the doublewrite file exist.

### Files to read
`engine/wal.h` (rules at the top), `wal.c` (`nx_wal_recover`); `engine.c`: `log_and_apply` (commit point), `checkpoint_locked`, `replay_log` / `replay_record`. Tests: `tests/unit/test_wal.c`, `test_recovery.c`.

### Likely examiner questions

**Q1. "The server is killed right after the log fsync but before the index is updated. Is the file there after restart?"**
Yes. The `COMMIT` record is durable, so recovery finds the transaction in the log and redoes it: `REF_INC` for each block, and `FILE_ADD`. The test case `txn.after_commit` does exactly this and checks the file reads back byte for byte. If the crash had come before the fsync, the record could be lost (on power loss) or kept (on a process crash), and both are consistent states.

**Q2. "Why do you need both a WAL and a doublewrite file?"**
They protect against different failures. The WAL protects *transactions*: committed changes not yet in the pages are redone. The doublewrite file protects *pages*: if a checkpoint is interrupted halfway through writing a page, that page is garbage, and there is nothing for the WAL to redo onto. We copy the intact version back from the doublewrite file first, then redo.

### More questions to practise
- Why may a record be applied twice during recovery without harm? *(Index changes compare the page LSN and skip; catalog changes are by key, so adding an existing file or deleting a missing one does nothing.)*
- Why is a damaged record in an *older* log segment treated differently from one at the end? *(A crash can only tear the very end of the log. Damage earlier means the disk lost data, and truncating there would silently throw away committed transactions, so the server refuses to start and reports it.)*
- Why do you never hand out an LSN at or below the checkpoint LSN after a crash? *(Pages may already carry LSNs up to it. A new change with a smaller LSN would be wrongly skipped as "already applied" by the next recovery.)*

## M6 — HTTP server, WebSocket, static files, and sign-in

### What it does
- **`net/http.c`** parses HTTP/1.1 request heads written from scratch, and writes responses with our security headers.
- **`net/server.c`** runs the server: one accept thread, a bounded queue, a pool of worker threads, and keep-alive (5 s idle, 100 requests per connection).
- **`net/router.c`** sends each request to static files, an API handler, or the WebSocket upgrade, after common gates.
- **`net/static.c`** serves the built React app from `web/dist`.
- **`net/ws.c`** implements WebSocket (RFC 6455) and the *hub* that pushes live events to browsers.
- **`api/auth.c`** handles join, login, logout, and `/api/me`, plus sessions and rate limits. **`api/app.c`** holds the running server's state, the join code, and a background checkpoint thread.
- **`main.c`** provides `--init`, `--create-admin`, `--fsck`, and serve.

### The life of one request
1. The accept thread takes the connection. If 64 are already waiting or being served, it answers `503` at once.
2. A worker reads bytes until the head is complete (at most 16 KB) and parses it strictly. A bad head is answered with 400, 411, 414, 431, 501, or 505, and the connection is closed.
3. The body is read only by `Content-Length` and only up to the route's limit (1 MB JSON, 8 MB uploads). A larger body is refused with 413 *before* it is read.
4. Gates, in order: the server must be ready (else 503); a mutating request must carry `X-NexusSync: 1` (CSRF); the session cookie is checked; the role is checked.
5. The handler answers. Bytes after the body that belong to the next pipelined request stay in the buffer.

### Why parse so strictly?
Everything that comes from the network is hostile until proven otherwise. Two examples:
- **Request smuggling.** Two different `Content-Length` values, or `Transfer-Encoding` next to `Content-Length`, can make a proxy and the server disagree about where one request ends. We refuse both.
- **Path traversal.** `/%2e%2e/secret` decodes to `/../secret`. Static paths are percent-decoded *first*, then refused if any segment is `.`, `..`, or starts with a dot, or the path contains `\` or `:`. The tests try six such tricks.

### Sessions and passwords
- Passwords are hashed with PBKDF2-HMAC-SHA256 and 600,000 iterations, so each guess costs about 0.2 s of CPU. A random 16-byte salt per user means two equal passwords give different hashes.
- A session is 32 random bytes, sent to the browser once as an `HttpOnly; SameSite=Strict` cookie. The server keeps only SHA-256 of the token. Page JavaScript cannot read the cookie (checked in a real browser), and other sites cannot make the browser send it.
- **Rate limits:** after 5 failed logins for one username, or 20 from one IP address within a minute, the answer is 429 before any hashing happens.

### WebSocket in one paragraph
The browser asks to upgrade `GET /ws` with a random key. The server proves it understood by returning base64(SHA-1(key + a fixed GUID)); that is the only use of SHA-1 in the project. After that, both sides exchange *frames*. Frames from the browser must be masked: they are XORed with a 4-byte key, so a malicious page cannot make the browser send bytes that a confused proxy might cache. The server sends JSON events like `{"type":"engine.checkpoint","at":...,"data":{...}}`. Only one thread (the broadcaster) writes to sockets, and each client has a bounded queue, so one slow laptop cannot block the others (D-024).

### OS and network concepts
- **Sockets, blocking I/O with timeouts, thread pools, a bounded producer/consumer queue (OS).**
- **Graceful shutdown:** waking threads blocked in `accept`/`recv` (socket shutdown, plus a self-connect on macOS).
- **HTTP/1.1 framing:** `Content-Length`, keep-alive, pipelining, `Expect: 100-continue`.
- **Web security:** CSRF, the SameSite and HttpOnly cookie flags, Content-Security-Policy, path traversal, cross-site WebSocket hijacking (we check `Origin`).
- **Password storage:** salt, slow hash, constant-time comparison.

### Files to read
`net/server.c` (`serve_connection`), `net/http.c` (`nx_http_parse_head`), `net/router.c` (`nx_route`: the gates), `api/auth.c` (`nx_h_login`), `net/ws.c` (`reader_main`, `broadcaster_main`). Tests: `tests/unit/test_net.c`, `tests/integration/test_server.c`.

### Likely examiner questions

**Q1. "How do you stop another website from making a student's browser delete their files?"**
Three layers. The session cookie is `SameSite=Strict`, so the browser does not attach it to requests that start on another site. Every mutating request must also carry the header `X-NexusSync: 1`; a cross-site form cannot set custom headers, and a cross-site `fetch` with one needs a CORS preflight we never approve. For WebSocket, the server compares the `Origin` header with `Host` and refuses mismatches.

**Q2. "Why not just give each WebSocket a worker thread like HTTP connections?"**
A WebSocket stays open as long as the tab, so 8 tabs would occupy all 8 workers and normal requests would wait forever. After the upgrade, the socket moves to the hub: a light reader thread per client and one shared broadcaster. Only the broadcaster writes, so two threads never interleave frames on one socket.

### More questions to practise
- What happens when a request has a 2 MB body to `/api/auth/login`? *(413 is sent before reading the body, and the connection is closed, because the unread body would be misread as the next request.)*
- Why does an unknown username still run PBKDF2? *(So the response time does not reveal whether the username exists.)*
- Why does the server keep only a hash of each session token? *(A memory dump or a log line then never contains a token that could be replayed.)*

## M7 — Uploads, downloads, the CLI, and concurrency

### What it does
- **`api/uploads.c`** implements the upload protocol. Only the blocks the server lacks cross the network.
- **`api/files.c`** lists, shows, downloads, shares, and deletes files, with the access rules.
- **`cli/`** is the `nexus` command-line client: `login`, `put`, `get`, `ls`, `rm`. It chunks files with the *same C code* as the server.
- **`scripts/concurrency_test.py`** and **`scripts/crash_test.py`** test the real programs: real processes, real sockets, real `kill -9`.

### The upload protocol, step by step
1. **Declare** (`POST /api/uploads`): name, size, whole-file SHA-256, chunker, number of blocks. The server checks that the numbers fit together (for example, a 200 KB file cannot have 3 CDC blocks, because no block is larger than 16 KB).
2. **Manifest** (`.../manifest?page=N`): the fingerprints and lengths of up to 8,192 blocks per page. The server looks each one up in the block index and answers `need: [...]`, the blocks it does not have. *This is where deduplication saves the network:* an identical file gets `need: []`.
3. **Chunks** (`.../chunks`): only the needed blocks, as `(index, length, bytes)` records. The server re-hashes every block and compares it with the manifest; mismatches are rejected one by one, with a reason.
4. **Commit**: the engine writes the recipe and commits the file (M4/M5). If a block is still missing, the answer is 409 with the indices, the client sends them, and commits again.

**Resume after a crash is free.** Blocks that reached the server before the crash are in the index, so a new upload of the same file is told it needs only the rest. The test sees a retry need 27 of 53 blocks.

### Who may see what
- A student reads their own files and files others shared; the admin reads everything.
- Only the owner or the admin may share or delete.
- A file you may not read answers **404, not 403**, so trying ids reveals nothing about other students' files.
- Upload sessions belong to their creator: someone else's upload id also answers 404.

### Concurrency: what happens when 8 students upload at once?
- Each worker thread serves one request. Hashing and container appends run in parallel; only the short commit step takes the transaction mutex (D-016).
- Two students uploading the same new block both send it. Both copies are appended, the second `CHUNK_ADD` finds the entry and does nothing, and the extra bytes are reclaimed by compaction (M12).
- `concurrency_test.py` runs 8 CLI processes at the same moment: 56 files (72 MB), every download verified by SHA-256, and `--fsck` clean afterwards.
- ThreadSanitizer found one real race while testing this milestone. Index statistics were read without a lock while a split changed them (D-033).

### Crash testing with real processes
`crash_test.py 50` runs random uploads and deletes from several CLI processes and kills the server at a random moment (0.1–3 s) in each of 50 rounds. After each kill:
- the restarted server must hold *exactly* the committed files, each with the right SHA-256;
- an upload that was in flight must be either completely there or not at all (atomicity);
- after a clean stop, `--fsck` must pass.

Result: 50/50 rounds clean, about 4,700 operations, more than 100 uploads interrupted mid-flight.

### Sessions now survive a crash (D-030)
The demo kills the server while students are signed in, and then they simply continue. So session hashes are saved to `data/sessions` (atomically, fsynced) before a login answers. Only hashes are stored, so the file is useless to a thief.

### OS and DBMS concepts
- **Atomicity and durability (DBMS):** an upload is all or nothing, even under `kill -9`.
- **Producer/consumer and thread pools (OS):** parallel uploads, with one short critical section.
- **Content addressing:** why deduplication and resume need no extra bookkeeping.
- **Defensive protocol design:** bounds on every length, index, and count; per-item validation.
- **Testing concurrent systems:** model-based checking, real-process fault injection, ThreadSanitizer.

### Files to read
`api/uploads.c` (`nx_h_upload_manifest`, `nx_h_upload_chunks`, `nx_h_upload_commit`), `api/files.c` (`nx_h_files_list`, `nx_h_file_content`), `cli/main.c` (`cmd_put`), `scripts/crash_test.py` (`verify`). Tests: `tests/integration/test_uploads.c`.

### Likely examiner questions

**Q1. "A student's laptop sends a block whose bytes do not match its fingerprint. What happens?"**
The server hashes every received block itself and compares the result with the fingerprint from the manifest. A mismatch is rejected (`hash_mismatch`) and never stored. If a client lied about the whole file, commit re-reads the file from storage, hashes it, and answers 422 (`file_hash_mismatch`). The server never trusts a client-supplied hash, length, or index.

**Q2. "The server is killed in the middle of an upload. What does the student see, and what is lost?"**
The upload fails. Nothing half-committed exists, because commit is atomic (WAL). Blocks that already reached the server stay in the index (or become orphan bytes if their `CHUNK_ADD` was not logged), and the student's session survives the restart. When they upload again, the manifest step tells them only the remaining blocks are needed. The crash test does this 50 times with real `kill -9`s.

### More questions to practise
- Why does the manifest come in pages of 8,192? *(One page is 288 KB, small enough for a single request; a 4 GB file needs about 100 pages, each answered with its own `need` list.)*
- Why answer 404 instead of 403 for another student's file? *(403 would confirm that the id exists; 404 reveals nothing.)*
- Why does `nexus get` write to a `.part` file first? *(If the download is cut short or the SHA-256 does not match, the half file is deleted and never mistaken for the real one.)*

## M8 — Frontend foundation

### What it does
- **Design tokens** (`web/src/styles/tokens.css`): every color, size, radius, and duration from DESIGN.md as CSS variables. A component never contains a raw value, so dark mode is just a second set of values.
- **Fonts:** Geist and Geist Mono, served by `nexusd` itself, because a lab network may have no internet.
- **API client** (`web/src/api/client.ts`): one place that adds the CSRF header, turns server errors into readable messages, and notices when the session has ended.
- **Live socket** (`web/src/api/ws.ts`): the WebSocket for events. It reconnects with growing pauses (1, 2, 4, 8, then 10 s).
- **The shell:** brand, navigation (admin pages only for admins), the live status ("Live, updated 3 s ago"), and the account menu (theme, sign out).
- **Sign in and join**, with errors placed under the field they are about.
- **System states:** offline banner (data dims but stays visible), recovering page, unreachable page, loading skeleton, "your session ended".

### How the app knows what is going on
1. **Is the server there?** `GET /api/health` at start, every second while it recovers, and with backoff when it cannot be reached.
2. **Who am I?** `GET /api/me`. The session cookie is HttpOnly (page scripts cannot read it, so stolen-script attacks cannot steal it), so asking the server is the only way.
3. **What is changing?** The WebSocket. If it drops, the status shows "Reconnecting", then "Offline" with a banner. When it comes back, every screen reloads its data, because it may have missed events.
4. **Did my session end?** Any 401 answer, or the socket being refused while the server answers health checks, means yes. The app goes to sign-in with a note.

### Accessibility, checked rather than assumed
- A computed contrast table for every text/background pair in both themes. It found two real failures in the original colors (input borders 1.66:1, red banner text 4.15:1); both are fixed and DESIGN.md updated (D-036).
- Keyboard: a "Skip to content" link first; the account menu opens with Arrow Down, moves with arrows, closes with Escape, and returns focus to its button; one visible focus ring everywhere.
- Screen readers: form errors are linked to their fields (`aria-describedby`) and announced; the live status announces only changes, not the ticking "3 s ago".

### Testing a UI against the real server
Playwright starts a real `nexusd` with a fresh data dir and drives Chrome at 1440×900, 1366×768, and 390×844. It also kills the server with SIGKILL in the middle of a test and checks that the app shows the offline banner, reconnects by itself, and is still signed in. Only two states are simulated, because a real server passes through them in milliseconds (D-037).

### OS and web concepts
- **Event-driven clients:** a socket plus reconnect with exponential backoff, and refetch after reconnect.
- **Web security in the browser:** HttpOnly and SameSite cookies, the CSRF header, the Content-Security-Policy (why the theme cannot be set by an inline script).
- **Accessibility as engineering:** WCAG contrast ratios, focus management, ARIA roles for menus.

### Files to read
`web/src/app/connection.tsx` (health loop and socket), `web/src/app/auth.tsx`, `web/src/app/App.tsx` (the gates), `web/src/styles/tokens.css`. Tests: `web/src/app/frontend.test.ts`, `web/e2e/foundation.spec.ts`.

### Likely examiner questions

**Q1. "The server crashes during the demo. What do the students' browsers do?"**
The WebSocket closes. The status turns to "Reconnecting", then "Offline" with a banner counting down to the next retry. The data on screen stays, dimmed. Meanwhile the app asks `/api/health`. If the server is replaying its log, the browser shows the recovering page; when the server is ready, it shows "Recovery complete" and reloads the data. Sessions are saved on the server's disk (M7), so nobody has to sign in again. A Playwright test does exactly this with a real `kill -9`.

**Q2. "Why not keep the login token in localStorage like many apps?"**
Anything in localStorage can be read by any script that runs on the page. An HttpOnly cookie cannot be read by scripts at all; the browser only sends it to our server. SameSite=Strict stops other sites from sending it, and the CSP stops foreign scripts from running in the first place.

### More questions to practise
- Why does the status say "updated 3 s ago" and not just "Live"? *(It shows how fresh the data is, measured from the last thing the server sent.)*
- Why is there no spinner while the page loads? *(DESIGN asks for content-shaped skeletons: the layout appears at once and nothing jumps when data arrives.)*
- How is dark mode implemented? *(The same token names get different values under `prefers-color-scheme: dark` or `data-theme="dark"`; components never change.)*
- Why does "Sign out" refuse to work while the server is unreachable? *(The session cookie is HttpOnly, so page scripts cannot delete it. Only the server can end the session. A browser-only sign-out would leave the next person at a shared lab PC signed in.)*


## M9 — Overview, Files, and the upload tray

### What it does
- **`GET /api/stats`** (`server/src/api/stats.c`): how much was uploaded (logical bytes), how much is on disk (physical bytes), writes skipped, network bytes saved, and, for the admin, engine internals (WAL, index, containers). Students get the same global numbers plus `mine`, their own share.
- **`GET /api/blockmap`**: the physical store cut into 384 equal slices ("tiles") in storage order. Each tile gets a shade 1–4 from how many files share its blocks. With `?file=ID` it answers which tiles hold that file's blocks; that drives the linked hover on the Overview.
- **The upload engine in the browser** (`web/src/engine/`): a Web Worker reads the file in 8 MB windows, cuts it into the same content-defined blocks as the server (parity-tested since M2), and hashes each block and the whole file. `upload.ts` then runs the protocol: create a session, send the manifest, send only the blocks the server says it needs (three requests at a time), commit.
- **Screens:** the Overview (headline, block field, recent uploads, engine summary or "Your storage"), Files (search, sort, scope, paging, a details drawer, sharing, delete with undo), and the upload tray with its phases: Reading → Checking → Sending → Verifying → Done.

### Why it is designed this way
- **The browser does the hashing, the server checks it.** Fingerprinting in the browser is what lets an identical file send 0 bytes. The server still re-hashes every block it receives and the whole file at commit, so a lying or broken client cannot corrupt anything (SPEC §13).
- **A Web Worker,** so a 1 GB file does not freeze the page: the main thread only draws progress.
- **Live tiles without an index walk.** Containers only grow at the end, so blocks stored a moment ago are the last bytes of the store. The tiles they landed in are simply the last `bytes_written` bytes. The test checks that this equals the file's real tiles (D-040).
- **The block map is cached.** Sorting the whole index into storage order costs a full walk, so it is rebuilt at most every 2 s, except when an answer must be fresh (a file's own tiles right after its upload).
- **Delete with undo sends nothing until the undo window is over.** Undo then never has to restore anything on the server; the DELETE goes out when the 6 s toast expires.
- **Honest numbers, even after a resume.** A retried upload sends only the missing blocks. Without care the server would then claim the first session's bytes were "never sent", but they were. The server keeps the sent-byte count of sessions that end without a commit and hands it to the next session for the same file (D-041). The browser test found this.

### OS and DBMS concepts
- **Content addressing and deduplication** made visible: the same block hash means the same bytes, stored once.
- **Storage order and physical layout:** the block map is literally the container files laid end to end.
- **Concurrency in the browser:** a worker thread, three parallel requests, and cancellation with AbortController. The first failure stops the other workers, so nothing changes after "failed" is shown. A unit test caught that bug.
- **Race conditions on the server:** a chunk request can finish after its session was cancelled. The byte count and the session's end are therefore updated under the same lock, in the documented lock order.

### Testing
- C: `test_stats_and_blockmap` (levels after one upload and after an identical copy; a second user's file maps to the last 96 tiles, exactly the tiles its live event named; private files 404; anonymous events for other students), plus resume and cancel accounting in `test_dedup_versions_resume`. Run under ASan and ThreadSanitizer.
- Web unit tests: fingerprint parity at three window sizes, the manifest and chunk wire formats, batching limits, and the upload job against a fake server (resume, rejected blocks, 409 retry, every failure kind, cancel). Plus every sentence the tray can show.
- Playwright (`web/e2e/m9.spec.ts`), against the real server: every state at three sizes. A real connection drop with resume; a corrupted chunk the real server rejects; a session ended from another tab; delete with undo; the SHA-256 in the drawer equals the downloaded file's hash; a second browser sees tiles pulse when someone else uploads.

### Files to read
`server/src/api/stats.c`, `web/src/engine/upload.ts`, `web/src/engine/fingerprint.ts`, `web/src/app/uploads.tsx`, `web/src/screens/Overview.tsx`, `web/src/screens/Files.tsx`.

### Likely examiner questions

**Q1. "How does the browser know which blocks to send?"**
It cuts the file into blocks and hashes each one in a Web Worker, then sends the list of (hash, length) pairs, the manifest, in pages of 8,192. The server looks each hash up in its extendible-hashing index and answers with the indices it does not have. Only those blocks are sent. An identical file therefore sends nothing but the manifest. An edited file sends only the blocks around the edit, because content-defined chunking puts the cut points back in the same places after it.

**Q2. "What does one tile in the block field mean?"**
The physical store, laid out in storage order (container files end to end), is cut into 384 equal slices. A tile's shade is the byte-weighted average of how many files reference the blocks in that slice: light for blocks written once, dark for blocks shared by five or more files. Hovering a recent upload asks the server which slices hold that file's blocks.

### More questions to practise
- Why does the server re-hash blocks the browser already hashed? *(Never trust the client: SPEC §13.)*
- Why are file-list deletes delayed by 6 s? *(Undo must not need a server-side restore; nothing is sent until the undo window closes.)*
- What happens if two people upload the same new file at the same moment? *(Both store their blocks; the second index insert finds the entry and is a no-op; the extra container bytes become orphan bytes that compaction (M12) reclaims.)*
- Why is `stats.updated` sent at most twice a second? *(Many uploads at once would otherwise flood every open browser; the payload is small and clients refetch details.)*

## M10 — Compare versions

### What it does
`GET /api/compare?a=&b=&mode=` (`server/src/api/compare.c`) cuts two files into blocks with the chosen method and reports, for version 2, which blocks version 1 already has. It returns the totals, the first difference, a window of 32 blocks with their fingerprints and storage locations, and a run-length "minimap" of the whole file. The Compare screen (`web/src/screens/Compare.tsx`) draws the two windows as strips: blue means already stored, peach means new. Hovering a version 2 block lights up its twin in version 1. Switching between "Fixed 4 KB blocks" and "Content-defined" replays the story.

### Why it is designed this way
- **It is the project's core idea in one picture.** Insert one line into a file. With fixed 4 KB blocks, every boundary after the insertion moves, so nothing after it matches (the test file: only 122 of 245 blocks match, 489 KB new). With content-defined blocks the cut points depend on the content, so they fall in the same places again: 210 of 211 blocks are already stored, and 4.7 KB is new.
- **Two ways to get the blocks.** If the file was stored with the method asked for, the blocks come straight from its recipe and know their container and offset. Otherwise the server reads the file through the engine (every block verified) and cuts it again in a streaming chunker, so a 1 GB file never needs 1 GB of memory (D-043).
- **A cache with reference counts.** Fixed mode reads both files in full, so results are cached for 5 minutes (4 entries). A result can be dropped from the cache while another request still uses it. A reference count, changed under one mutex, makes sure it is freed only by the last user. A test with six threads and six comparisons (more than the four slots) checks this under ThreadSanitizer.

### OS and DBMS concepts
- **Content-defined chunking** (Rabin/Gear rolling hash) against fixed-size blocks: the boundary-shift problem.
- **Hash joins:** matching blocks is a hash join of two lists on the SHA-256 key (`nx_hmap32`), linear time instead of comparing every pair.
- **Run-length encoding** for the minimap: a 1,500-block file becomes a handful of runs.
- **Reference counting and safe reclamation** in a shared cache.
- **A real data race found by tooling:** cJSON writes a global error variable on every parse. ThreadSanitizer caught two worker threads doing so at once, and server-side parsing now goes through one small locked wrapper (D-044).

### Testing
- C: `test_compare` (content-defined: at most 3 new blocks; fixed: exactly the 36 whole blocks before the edit match, and the first block's hash equals the SHA-256 of the first 4 KB; window defaults and clamping; minimap runs add up; access rules; nine malformed queries), plus the six-thread cache test, under ASan and ThreadSanitizer.
- Playwright (`web/e2e/m10.spec.ts`): defaults, both modes with their numbers, hover partners, the details panel (including the "calculated, not stored" note and a fingerprint checked against Node's SHA-256), minimap click and keyboard, opening from the Files drawer, the error state, dark mode, and reduced motion.

### Likely examiner questions

**Q1. "Why does one inserted line ruin fixed-size deduplication?"**
Fixed blocks start every 4,096 bytes from the start of the file. Inserting 21 bytes moves every later byte 21 places along, so every later block holds different bytes and gets a different hash, even though the content is the same. Content-defined chunking cuts where a rolling hash of the last few bytes hits a pattern. After the insertion that same content produces the same hash values, so the cuts come back to the same content positions, and only the block containing the edit changes.

**Q2. "How does the server find which blocks of version 2 exist in version 1 without comparing every pair?"**
It puts version 1's block hashes in a hash table (hash → first index), then looks up each version 2 hash once: O(n + m) instead of O(n × m). This is a hash join.

### More questions to practise
- Why does the details panel say "used 6 times" and not "shared by 6 files"? *(The index stores a reference count; one file can reference a block twice; counting distinct files would mean reading every recipe.)*
- Why are comparisons over 1 GB refused? *(Fixed mode reads both files in full; one request must not keep a worker busy for minutes.)*
- Why did fixed mode report 122 matching blocks and not 0? *(Blocks before the insertion are unchanged; only those after it shift.)*

## M11 — Engine and Users screens, system states

### What it does
- **Engine** (`web/src/screens/Engine.tsx`): the storage engine, live.
  - Server facts and LAN addresses with QR codes, so other laptops can connect; the join code.
  - The write-ahead log (current LSN, records since the last checkpoint, "Checkpoint now").
  - The block index: entries, directory depth, bucket fill, and the buffer pool hit rate with a 60 s sparkline.
  - Containers, with a histogram of how full each one is.
  - Integrity, the recovery history (every start, "clean" or "recovered"), and the event log.
- **Users** (`web/src/screens/Users.tsx`): everyone with their files and storage. An admin can make someone an admin or a student, and disable or enable them; disabling signs that person out everywhere at once. The join code can be copied or rotated.
- **Admin API** (`server/src/api/admin.c`): users, user changes, events, recovery history, and addresses.
- **System states:** while recovering, the server counts records replayed so far, and the page shows that count. Offline now dims only charts, not text.

### Why it is designed this way
- **Recovery history must survive a crash,** because a crash is exactly what it records. Each start appends a fixed 64-byte record with its own CRC to `data/recoveries` and fsyncs it. A record torn by a crash fails its CRC and is skipped. The same idea as the WAL, in miniature.
- **The event log is kept in memory, in a ring** (a circular buffer of the last 256 events). It is for watching live; writing every event to disk would add an fsync per event for no gain.
- **The hit-rate sparkline uses raw counters.** A rate since start barely moves after a while, so the server sends hits and misses, and the browser charts the rate per 2 s interval.
- **An admin cannot demote or disable themself,** so a lab can never lock itself out from the web.
- **QR codes are drawn as SVG paths,** coloured from tokens and dark-on-light in both themes, because phone cameras read that most reliably.

### OS and DBMS concepts
- **Durable append-only logs with per-record checksums** (the recovery history), and a ring buffer (the event log).
- **Buffer pool hit rate:** how often a page lookup finds the page already in memory. With a no-steal policy, a full pool of dirty pages forces a checkpoint, and the "Dirty pages" number shows that pressure.
- **Extendible hashing** made visible: global depth and bucket count grow as blocks arrive.
- **Session revocation:** disabling a user deletes their session hashes from `data/sessions` and closes their WebSockets.

### Likely examiner questions

**Q1. "You kill the server during the demo. What does the Engine screen show afterwards, and how do you know it is true?"**
The recovery history's newest row says "Recovered", with the number of log records replayed and transactions redone. Those numbers come from the engine's replay loop at start-up. They are written to `data/recoveries` with a CRC before the server reports ready. The event log's first line is the same `engine.recovery` event. A Playwright test does exactly this with a real SIGKILL.

**Q2. "Why is the hit rate high, and when would it drop?"**
The block index's bucket pages for a small lab fit in the 16 MB buffer pool, so almost every lookup hits memory. It drops when the index grows past the pool (many millions of blocks) or right after a restart, when the pool is cold. The sparkline shows the rate per 2 s interval, so a burst of misses after a restart is visible.

### More questions to practise
- What happens to a disabled student's open browser tab? *(Its WebSocket is closed and the next request gets 401, so it shows "Your session ended".)*
- Why were "Run cleanup" and "Scan now" disabled in M11? *(They were built in M12; until then the server answered 501 and the screen said so. See the M12 section.)*
- Why does rotating the join code not sign anybody out? *(The code is only checked when an account is created; sessions are separate.)*

## M12 — Cleanup, compaction, integrity scan, index rebuild

### What it does
- **Cleanup** (`server/src/engine/gc.c`, `nx_engine_gc`) has two steps.
  - *Freeing:* every block that no file uses (refcount 0) and that is older than `gc_grace_s` (1 hour) loses its index entry, through a `BLOCK_FREE` log record. Blocks that an open upload has declared are kept.
  - *Compaction:* a container whose live data is below 50% gets its live blocks copied into a fresh container. The moves are logged with `BLOCK_MOVE`, and the old file is deleted after the next checkpoint.
- **Integrity scan** (`server/src/engine/scan.c`, `nx_engine_scan`): it reads every stored block and checks three things: the record's CRC-32, the hash stored in the record, and SHA-256 of the data. For each damaged block it names the files that use it.
- **`--rebuild-index`:** if the block index is lost or damaged, the server rebuilds it from the containers (every record carries its block's hash) and the recipes (which give the reference counts).
- **Jobs and schedule** (`server/src/api/maint.c`): an admin starts a job with `POST /api/admin/gc` or `/scan` (answer 202), or the nightly run starts it at 02:00 local time. The job runs on its own thread and reports through `engine.gc` and `engine.scan` events. The Engine screen shows the progress and the result in words.

### Why it is designed this way
- **Freeing and moving are logged transactions,** like every other change. The crash and recovery code from M5 therefore covers cleanup with no new recovery logic: recovery redoes `BLOCK_FREE` and `BLOCK_MOVE` like any other record.
- **The order of compaction steps is the safety argument:** copy and fsync, then log the moves, then checkpoint, then delete. A crash before the commit leaves only extra copies, which are harmless orphan bytes. A crash after the commit is fixed by redo. A crash after the checkpoint leaves an old file with nothing live in it, and the next cleanup deletes it. The tests crash at each of these points.
- **The store barrier.** An upload writes a block into a container *before* it logs `CHUNK_ADD`. If compaction deleted that container in between, the log would point into a missing file. Compaction therefore waits until every upload that started before it has logged its blocks. That is an epoch counter, the same idea as RCU in the Linux kernel. Uploads never wait for compaction. A test shows the data loss when the barrier is switched off.
- **A damaged live block is never moved.** Moving it would copy the damage and hide where it came from, so its container stays as it is for the scan to report.
- **Jobs run on their own thread,** because a scan reads every stored byte and may take minutes, longer than a browser waits for an HTTP answer.

### OS and DBMS concepts
- **Garbage collection by reference counting,** with a grace period. This is the mark-free cousin of what file systems and object stores do with unreferenced extents.
- **Compaction (segment cleaning),** as in log-structured file systems (LFS) and LSM trees: rewrite mostly-empty segments to get the space back.
- **Write ordering for crash safety:** "copy, fsync, log, checkpoint, delete" is the same discipline as "write data before the commit record".
- **Synchronization:** a mutex plus condition variable used as a barrier between two kinds of work, with a defined lock order (SPEC §9).
- **Derived data:** the index can be rebuilt from the base data, the way a database rebuilds a secondary index.

### Likely examiner questions

**Q1. "The server is killed while compaction is moving blocks. Can a file lose data?"**
No. The old container is deleted only after the moves are committed in the log and a checkpoint has written them to the index pages. Before the commit, the old locations are still valid. After it, recovery redoes the moves. `test_compaction_crash_points` kills the engine at ten points (during the frees, mid-copy, after the copy, after one or three containers' moves, mid-apply, during the checkpoint, after it, and between deletions). Each time fsck is clean, every file is byte-exact, and the next cleanup finishes the job. `crash_test.py` also runs cleanup during real SIGKILLs of the server.

**Q2. "How does cleanup know a block with refcount 0 is really garbage, and not part of an upload that is still going on?"**
Two protections. First, the grace period: a block is freed only if it was stored more than `gc_grace_s` ago. Second, the upload session's declared hashes: cleanup asks `nx_uploads_declares` for each candidate. Both are checked again under the transaction mutex just before freeing. If an upload still loses a block in a narrow race, its commit finds the block missing and answers 409, and the client sends the block again; nothing is lost.

### More questions to practise
- Why 50%? *(Rewriting a container costs copying its live part. Below half live, the copy is smaller than the space it frees.)*
- Why does the scan keep its last result in a file? *(The demo kills the server; the Engine screen should still say when the last scan ran.)*
- What does `--rebuild-index` do with blocks no file uses? *(It indexes them with refcount 0; the next cleanup frees them.)*
