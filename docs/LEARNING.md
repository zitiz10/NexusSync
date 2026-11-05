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

