# NexusSync — technical specification

Version 1.9 (1.9: cleanup, compaction, scan, and rebuild details, the store barrier and maintenance mutex in §9, `data/scan_last`, `gc`/`scan` answer 202 with progress events, new `cleanup`/`integrity` stats fields (D-048). 1.8: admin API shapes, `data/recoveries` file, health `recordsReplayed`, stats `server` (D-046). 1.7: `/api/compare` shape written down (files, window count, minimap pairs, defaults, limits; D-043). 1.6: `/api/stats` null and admin-only rules, `/api/blockmap` shading and extra fields, anonymous `upload.progress` copies for other students (D-040); a resumed upload's `bytesSent` includes earlier sessions (D-041). 1.5: sessions persisted in `data/sessions` (D-030); upload limits and validation rules; the files list shows version-chain heads; CLI environment variables. 1.4: `data/join_code` file; `/api/health` progress shape; static app served while recovering; username and join-code rules written down in §13. 1.3: WAL segment files get a 16-byte header; recovery cut rules and the unsynced `CHECKPOINT` record written down in §5 and §8. 1.1: server portable to Linux, macOS, and Windows; in-repo crypto replaces OpenSSL; scripts in Python. 1.2: `recipes.dat` gains a 16-byte file header; superblock byte offsets and the doublewrite format written down; one transaction mutex replaces the checkpoint rwlock and the 256 index stripes in §9, see DECISIONS D-016. No WAL, chunker, or API changes). Source of truth for behavior, formats, and the API. Section numbers are referenced from `CLAUDE.md`; keep them stable.

## 1. Goals and non-goals

Goals: store files with block-level deduplication across all users; send only missing blocks over the network; never lose or corrupt committed data, including after `kill -9` or power loss; show real engine state live; run on one machine (Linux, macOS, or Windows) serving browsers on the same LAN.

Non-goals: see `PRODUCT.md` "Out of scope".

Correctness invariants (tests and `--fsck` check all of them):
- **I1** Every block referenced by a committed file recipe exists in the index and its bytes in a container hash to its key.
- **I2** Every index entry's refcount equals the number of references to that block across all committed, non-deleted file recipes (a block appearing twice in one file counts twice).
- **I3** Downloading a committed file returns exactly the bytes uploaded (whole-file SHA-256 matches the stored value).
- **I4** After recovery, the state equals the state after the last committed transaction before the crash. Uncommitted work leaves only unreferenced blocks or orphan container bytes, which cleanup reclaims.

## 2. Architecture

```
 Browser (any laptop)                              nexusd (server laptop: Linux/macOS/Windows)
 ┌──────────────────────┐   HTTP/1.1 + WebSocket   ┌────────────────────────────────────────┐
 │ React app            │◄────────────────────────►│ net: accept thread → worker pool       │
 │ Web Worker:          │                          │      http parser, router, ws, static   │
 │  chunk (CDC) + SHA-256│                         │ api: auth files uploads compare stats  │
 └──────────────────────┘                          │      admin events                      │
 CLI `nexus` (C) ─────────────── HTTP ────────────►│ engine:                                │
                                                   │  chunkers, sha                         │
                                                   │  block index (extendible hashing)      │
                                                   │  buffer pool (clock)                   │
                                                   │  containers (append-only, 4 MB)        │
                                                   │  catalog (users, files) + recipes      │
                                                   │  WAL, checkpoint, recovery             │
                                                   │  gc/compaction, integrity scan         │
                                                   └───────────────┬────────────────────────┘
                                                                   │ files on local SSD
                                                                   ▼  data/
```

One process, `nexusd`. Threads: one accept thread, a fixed worker pool (default 8) that handles one connection at a time each, one WebSocket broadcaster, one background thread for checkpoints, cleanup, and scans.

## 3. Configuration

`nexussync.conf` (INI style, `key = value`, `#` comments). Example in `nexussync.conf.example`.

| Key | Default | Meaning |
|---|---|---|
| `listen` | `0.0.0.0:8080` | Address and port |
| `data_dir` | `./data` | Storage root |
| `web_root` | `./web/dist` | Static files |
| `workers` | `8` | Worker threads |
| `max_connections` | `64` | Further connections get 503 |
| `chunker` | `cdc` | `cdc` or `fixed` for new uploads (stored per file) |
| `container_size` | `4194304` | Bytes before a container is sealed |
| `buffer_pool_pages` | `4096` | 4 KB frames (16 MB) |
| `checkpoint_interval_s` | `60` | Also triggered when dirty frames exceed 50% |
| `gc_grace_s` | `3600` | Unreferenced blocks younger than this are kept |
| `verify_on_read` | `true` | Re-hash blocks during download |
| `verify_on_commit` | `true` | Re-read and hash the whole file at commit |
| `upload_session_ttl_s` | `1800` | Idle upload sessions expire |
| `session_ttl_s` | `43200` | Login sessions (12 h) |
| `join_code` | random on `--init` | Required to create an account |

## 4. On-disk layout

```
data/
  superblock.a  superblock.b     two alternating copies, newest valid wins
  containers/c00000001.dat ...   append-only block containers
  index/dir.bin                  extendible-hashing directory
  index/buckets.bin              bucket pages (4096 bytes each)
  catalog/users.tbl              slotted pages
  catalog/files.tbl              slotted pages
  recipes/recipes.dat            append-only recipe records
  wal/wal.00000001 ...           log segments (64 MB each)
  doublewrite.bin                checkpoint page staging
  join_code                      current join code (one line), replaced atomically on rotation
  sessions                       SHA-256 of each session token + user + expiry (mode 0600), replaced atomically
  recoveries                     one 64-byte record per start ("NXRV", CRC per record), appended (M11, D-046)
  scan_last                      last integrity scan: "NXSN", u16 1, u16 0, u64 at_ms, u64 verified, u64 failures, u32 crc32 over the first 32 bytes (36 bytes), replaced atomically (M12, D-048)
```

All integers little-endian, fixed width. Every format starts with a 4-byte magic and a `u16` version.

## 5. Formats

**Superblock (4096 bytes):** magic `NXSB`, version, `u64 seq`, `u64 checkpoint_lsn`, `u64 next_file_id`, `u32 next_user_id`, `u32 next_container_id`, `u32 active_container_id`, `u32 index_global_depth`, `u32 bucket_page_count`, chunker params (`u32 min, avg, max`, `u32 mask_s, mask_l`, `u32 gear_seed_lo, gear_seed_hi`), `u64 created_at`, `u32 crc32` over all preceding bytes. Writing alternates `.a` and `.b`; recovery uses the valid copy with the higher `seq`.

**Container file:** header 64 bytes (magic `NXSC`, version, `u32 id`, `u64 created_at`, padding). Then block records: `u8 hash[32]`, `u32 len`, `u32 crc32(data)`, `data[len]`. The hash is stored inline so the index can be rebuilt from containers alone (`--rebuild-index`). Sealed when appending the next record would exceed `container_size`.

**Bucket page (4096 bytes):** header 16 bytes (`u64 page_lsn`, `u16 local_depth`, `u16 count`, `u32 crc32`). Entries of 52 bytes: `u8 hash[32]`, `u32 container_id`, `u32 offset`, `u32 len`, `u32 refcount`, `u32 created_at` (seconds since epoch, needed for the cleanup grace period). Capacity 78 entries per page.

**Directory (`dir.bin`):** magic `NXDR`, version, `u32 global_depth`, then `2^global_depth` × `u32 page_id`, then `u32 crc32`. Rewritten whole at checkpoint via the doublewrite path.

**Catalog tables:** 4096-byte slotted pages: header (`u64 page_lsn`, `u16 slot_count`, `u16 free_offset`, `u32 crc32`), slot array (`u16 offset`, `u16 length`, length 0 = free), records growing from the end.
- `users` record: `u32 id`, `u8 role` (0 student, 1 admin), `u8 status` (0 active, 1 disabled), `u16 username_len`, username, `u16 display_len`, display name, `u8 salt[16]`, `u8 pw_hash[32]`, `u32 pbkdf2_iters`, `u64 created_at`, `u64 last_seen`.
- `files` record: `u64 id`, `u32 owner_id`, `u16 name_len`, name (UTF-8, max 255 bytes), `u32 version`, `u64 prev_version_id` (0 if none), `u64 size`, `u32 chunk_count`, `u8 chunker`, `u64 recipe_offset`, `u32 recipe_len`, `u8 file_sha256[32]`, `u64 bytes_sent`, `u64 created_at`, `u8 flags` (bit0 shared, bit1 deleted).

**Recipe file (`recipes.dat`):** a 16-byte file header (magic `NXRF`, `u16` version 1, `u16 0`, 8 zero bytes), then records. **Recipe record:** magic `NXRC`, `u64 file_id`, `u32 chunk_count`, then `chunk_count` × (`u8 hash[32]`, `u32 len`), then `u32 crc32` over the record's preceding bytes. Appended and `fsync`ed before the commit record that references it. A torn record at the tail is left in place (it is unreachable) and new records follow it.

**Superblock byte offsets:** magic 0, version 4, `seq` 8, `checkpoint_lsn` 16, `next_file_id` 24, `next_user_id` 32, `next_container_id` 36, `active_container_id` 40, `index_global_depth` 44, `bucket_page_count` 48, chunker params 52–79, `created_at` 80, `crc32` 88 (over bytes 0–87), zero padding to 4096. Copy `.a` holds odd `seq`, `.b` even.

**Doublewrite file (`doublewrite.bin`):** header 16 bytes (magic `NXDW`, `u16` version, `u16 0`, `u64 checkpoint_lsn`); entries (`u32 file_id`, `u32 page_no`, `page[4096]`, `u32 crc32` over the entry's preceding 4104 bytes); trailer 12 bytes (magic `NXDT`, `u32 count`, `u32 crc32` over every preceding byte of the file). File ids: 0 `buckets.bin`, 1 `users.tbl`, 2 `files.tbl`, 3 `dir.bin`. Empty (length 0) when no checkpoint is in progress. A batch that is not complete and valid is ignored.

**WAL segment file (`wal.NNNNNNNN`):** header 16 bytes (magic `NXWL`, `u16` version, `u16 0`, `u64 first_lsn`), then records. LSNs are consecutive across the whole log, and a segment's first record carries its `first_lsn`. A transaction (`BEGIN` … `COMMIT`) is written with one write and never split across segments; a segment is fsynced before the next is created, so only the newest segment can end in a torn record. `txn_id` is the LSN of the transaction's `BEGIN`; `CHECKPOINT` records have `txn_id` 0, are written outside transactions, and are not fsynced on their own (the next commit's fsync makes them durable). Payloads are at most 4096 bytes.

**WAL record:** `u64 lsn`, `u64 txn_id`, `u8 type`, `u32 payload_len`, payload, `u32 crc32` over all preceding fields of the record. Types:

| Type | Payload |
|---|---|
| `BEGIN` | none |
| `CHUNK_ADD` | hash[32], container u32, offset u32, len u32, created_at u32 |
| `REF_INC` / `REF_DEC` | hash[32], `u32 count` |
| `FILE_ADD` | full files record |
| `FILE_UPDATE` | file id, flags |
| `FILE_DEL` | file id |
| `USER_ADD` / `USER_UPDATE` | full users record |
| `BLOCK_FREE` | hash[32] |
| `BLOCK_MOVE` | hash[32], new container u32, new offset u32 |
| `CONTAINER_DROP` | container id |
| `COMMIT` | none |
| `CHECKPOINT` | checkpoint_lsn |

## 6. Chunking and hashing

### 6.1 Fixed-size chunker
4096-byte blocks; the last block holds the remainder.

### 6.2 Content-defined chunker (FastCDC style, 32-bit Gear hash)
Parameters (part of the format; changing them is a format version bump): `MIN = 2048`, `AVG = 4096`, `MAX = 16384`, `MASK_S = 0xFFFC0000` (14 bits, used before `AVG`), `MASK_L = 0xFFC00000` (10 bits, used after `AVG`).

```
cut(buf, n):
  if n <= MIN: return n
  normal = min(AVG, n); limit = min(MAX, n)
  h = 0 (uint32); i = MIN
  while i < normal: h = (h << 1) + GEAR[buf[i]]; if (h & MASK_S) == 0: return i + 1; i++
  while i < limit:  h = (h << 1) + GEAR[buf[i]]; if (h & MASK_L) == 0: return i + 1; i++
  return limit
```

All arithmetic is unsigned 32-bit (in TypeScript use `>>> 0` after every step). `GEAR` is 256 `uint32` values generated by `scripts/gen_gear.py` with SplitMix64 from seed `0x4E657875735379` taking the upper 32 bits of each output; the script writes both `server/src/engine/gear_table.h` and `web/src/engine/gearTable.ts`. Report the measured average chunk size in the evaluation (it will be somewhat above 4 KB because of `MIN`).

### 6.3 Hashing
SHA-256 for blocks and whole files. Server and CLI use the in-repo implementation in `server/src/crypto/` (FIPS 180-4), tested against NIST vectors and against Python `hashlib` on random inputs. The browser uses `hash-wasm` inside a Web Worker, because `crypto.subtle` is only available in secure contexts and LAN clients connect over plain HTTP. Files are read with `Blob.slice` in 8 MB windows; the chunker carries state across windows; memory stays bounded.

### 6.4 Parity tests
`server/tests/fixtures/` holds 10 deterministic test files (sizes 0, 1, 2047, 2048, 4096, 16385, 1 MB, 10 MB random, 10 MB text, and the 10 MB text with 300 bytes inserted at offset 5000) and the expected cut points and hashes produced by the C code. Vitest asserts the TypeScript chunker produces identical results. Any mismatch blocks the build.

Storage: the 33 MB of fixture data is not committed. `scripts/make_fixtures.py` regenerates it byte-identically into `server/tests/fixtures/data/` (git-ignored); `make test` and the vitest global setup run it when missing. Committed: `chunks_expected.txt` (written by `make fixtures-expected` from the C chunkers: per file its size and SHA-256, the CDC block lengths, and per method the block count and a digest = SHA-256 over the concatenated block hashes) and `crypto_vectors.txt` (SHA-256, SHA-1, HMAC, PBKDF2, CRC-32 values from Python's `hashlib`/`hmac`/`zlib`). The text fixtures are lab-style CSV; the "insert" fixture adds 300 pseudo-random bytes at offset 5000. CI also runs `scripts/gen_gear.py --check` so the C and TS Gear tables cannot drift.

## 7. Storage engine

### 7.1 Containers
One active container receives appends under a mutex. The active container is the highest-numbered container file; a damaged tail (interrupted append) is never appended after: that container is sealed as is and a new one starts (DECISIONS D-014). Each append writes the record, and blocks are made durable in batches: the uploader collects up to 64 accepted blocks or 50 ms, calls `fsync` on the container once, then logs one `CHUNK_ADD` transaction for the batch. On startup the active container's append offset is its file length; trailing bytes from an interrupted write are orphan bytes, reclaimed by compaction.

### 7.2 Block index: extendible hashing
Directory of `2^global_depth` page ids, indexed by the top `global_depth` bits of the block hash. Lookup: one directory read (in memory) and one bucket page read (through the buffer pool). Insert into a full bucket: if `local_depth < global_depth`, split the bucket into two pages by the next bit and repoint directory slots; otherwise double the directory first. Buckets never merge (documented choice). Start with `global_depth = 4`.

### 7.3 Buffer pool
`buffer_pool_pages` frames of 4096 bytes, Clock replacement, per-frame latch and pin count. **No-steal:** a dirty frame is never written outside a checkpoint. When dirty frames exceed 50% of the pool, wake the checkpointer; if no frame can be evicted, the requesting thread waits for the checkpoint to finish. Track hits and misses for the Engine screen. Every page (index buckets, catalog pages) begins with `u64 page_lsn` and has its CRC at bytes 12–15, so the pool checksums all pages the same way and redo can skip changes a page already contains (DECISIONS D-013).

### 7.4 Catalog and recipes
Users and files live in slotted-page tables through the same buffer pool, so they share the checkpoint and recovery path. On startup, build in-memory indexes: users by id and username; files by id, by owner, and by (owner, name) for versions. Recipes are append-only; deleted files leave dead recipe bytes, reclaimed by a stretch-goal compaction.

### 7.5 Engine API (internal, `include/engine.h`)
`engine_open`, `engine_close`, `engine_lookup(hash)`, `engine_store_blocks(batch)`, `engine_commit_file(meta, chunks)`, `engine_delete_file(id)`, `engine_read_file(id, writer_cb)`, `engine_stats`, `engine_checkpoint`, `engine_gc`, `engine_scan`, `engine_fsck`. The HTTP layer never touches files directly.

## 8. Transactions, logging, checkpoint, recovery

Model: **redo-only logging, no-steal, no-force**. Changes to pages are applied in the buffer pool only after the transaction's `COMMIT` record is durable. Because dirty pages reach disk only during checkpoints, the on-disk pages always equal the state at the last checkpoint, and recovery is "replay committed transactions after the checkpoint". No undo is needed.

Transactions:
- **Store blocks:** for each verified new block: `CHUNK_ADD` (refcount 0). `COMMIT`. Container `fsync` happens before the WAL append.
- **Commit file:** append and `fsync` the recipe; take the transaction mutex; verify every chunk exists (missing → abort with 409 and the list); log `BEGIN`, one `REF_INC` per distinct hash with its count, `FILE_ADD`, `COMMIT`; `fsync` the WAL; apply to pages; unlock.
- **Delete file:** `FILE_DEL`, `REF_DEC` per distinct hash, `COMMIT`.
- **User changes:** `USER_ADD` or `USER_UPDATE`, `COMMIT`.
- **Cleanup:** `BLOCK_FREE` for unreferenced blocks older than `gc_grace_s` and not declared by any open upload session; `COMMIT`.
- **Compaction:** copy live blocks of containers below 50% live data into a fresh container, `fsync`, log `BLOCK_MOVE`s and `CONTAINER_DROP`, `COMMIT`; delete the old container file only after the next checkpoint.

**What `fsync` means on each OS.** Every `fsync` in this spec is `plat_fsync_file()` in the platform layer:
- Linux: `fsync()`, which flushes the file and asks the drive to flush its write cache.
- macOS: `fcntl(fd, F_FULLFSYNC)`. A plain `fsync()` on macOS only hands data to the drive and does not flush the drive's cache, so it is not enough for a commit point. If `F_FULLFSYNC` is refused (some network filesystems), fall back to `fsync()` and log a warning once.
- Windows: `FlushFileBuffers()` on the file handle.

Creating, renaming, or deleting a file also needs the directory entry made durable: `plat_fsync_dir()` opens the directory and fsyncs it on Linux and macOS; on Windows it does nothing because NTFS journals metadata changes. Atomic replace uses `rename()` on POSIX and `MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)` on Windows. On Windows, data files are opened with `FILE_SHARE_DELETE` so a WAL segment or container can be deleted while another handle still has it open.

`kill -9` tests (SIGKILL, or TerminateProcess on Windows) prove crash safety of the process. Real power loss additionally depends on the drive honouring the flush; consumer drives with volatile caches may not. This is a documented limitation.

**Commit point:** a transaction is committed once its `COMMIT` record is written and the WAL is `fsync`ed. Group commit (one `fsync` for several waiting transactions) is a stretch goal.

**Checkpoint:**
1. Take the transaction mutex (writers hold it for the duration of logging and applying a transaction, so a checkpoint never sees half of an unrelated transaction; a checkpoint started *during* an apply because the pool is full uses the LSN before that transaction's `BEGIN`, D-013).
2. Record `L` = last durable LSN. Collect all dirty frames and the directory.
3. Write them to `doublewrite.bin` (page id, file, bytes, crc each, then a trailer with count and crc); `fsync`.
4. Write each page in place; `fsync` each file.
5. Write the next superblock copy with `checkpoint_lsn = L`; `fsync`.
6. Mark `doublewrite.bin` empty; `fsync`. Mark frames clean. Delete WAL segments entirely below `L`.
7. Release the lock. Log a `CHECKPOINT` record and emit an `engine.checkpoint` event with duration and pages written.

**Recovery (on every start):**
1. Load the newest valid superblock.
2. If `doublewrite.bin` holds a complete, valid batch, copy its pages in place and `fsync` (repairs a torn checkpoint).
3. Scan WAL from `checkpoint_lsn`. Stop at the first record with a bad CRC, short length, or non-consecutive LSN. In the newest segment that is a torn tail; anywhere else it is damage, and the server refuses to start rather than drop committed transactions. Cut the log back to the end of the last `COMMIT` or `CHECKPOINT` record (this also removes a complete transaction that never committed); the next LSN continues from there, and is never at or below `checkpoint_lsn`.
4. Group records by transaction; redo only committed ones, in LSN order, through the normal apply functions without logging.
5. Run a checkpoint. Rebuild in-memory indexes. Emit `engine.recovery` with records replayed, transactions redone, duration.
While recovering, `/api/health` returns `{"status":"recovering","progress":{"phase":"replaying-log","elapsedMs":N}}` and every other `/api` endpoint and `/ws` return 503 with `Retry-After: 5`. The static web app is still served, so the browser can show the recovering state (DECISIONS D-026).

## 9. Concurrency and lock order

Locks, always acquired in this order and released in reverse:
1. Transaction mutex (one transaction is logged and applied at a time; checkpoints take it too). Replaces v1.1's checkpoint rwlock and 256 index stripes (DECISIONS D-016)
2. Catalog lock (rwlock over users and files tables and in-memory indexes)
3. Index directory rwlock (shared for lookups and applies, exclusive only to split or double)
4. *(removed in v1.2: index stripes)*
5. Buffer pool frame latches (short, never held across I/O except the page read itself)
6. Active container mutex
7. WAL mutex

Before all of these: the **maintenance mutex** (`maint_mu` in `engine.c`, M12) serializes cleanup, compaction, and the integrity scan; it is taken with no other lock held and held for the whole job. The **store barrier** (`gate_mu`, M12, D-048) is a leaf: `nx_engine_store_blocks` takes it only to register before its first container append and to deregister after its `CHUNK_ADD` commits; compaction takes it (holding only `maint_mu`) to wait until every store registered before a given moment has finished.

The upload table and session mutexes in `api/uploads.c` are never held while calling into the engine, so cleanup may call `nx_uploads_declares` while holding the transaction mutex (D-048).

Leaf locks (taken last, never while acquiring another lock, held only for short work): the buffer pool's page-table mutex (taken inside pin/unpin/mark-dirty while a frame latch may be held; it may do the page read on a miss), the container read-handle cache mutex, the recipe append mutex (held across the recipe write and its `fsync`), the log output mutex, the block-map cache mutex in `api/stats.c` (held only to read or swap the cached storage layout; never held across an engine call, M9), the compare cache mutex in `api/compare.c` (held only to find, insert, or release a cached result; M10), and the JSON parse mutex in `util/json_in.c` (held only while cJSON parses a request body: cJSON writes a global error position, D-044). Engine statistics are atomic counters, not locked.

Readers (block lookups, downloads, `GET` handlers) never take the transaction mutex, except to run a checkpoint after `NX_BP_FULL`, and then they hold no other lock. Uploads append blocks to containers without the transaction mutex and take it only to log their `CHUNK_ADD` batch.

Index operations never wait for a split while holding the shared directory lock: an insert into a full bucket returns `NX_EXT_NEED_SPLIT`, the caller releases the shared lock, takes it exclusively, splits, and retries. A split is not logged and keeps the old page LSN on both pages (D-018). A full buffer pool returns `NX_BP_FULL` instead of blocking; the caller releases its locks and runs a checkpoint (DECISIONS D-013).

Rules: never hold an engine lock while waiting on network I/O; never call into the HTTP layer while holding engine locks; the WebSocket broadcaster receives events through a bounded queue. Two uploads storing the same new block: the second `CHUNK_ADD` to be applied finds the entry and is a no-op; its container copy becomes orphan bytes reclaimed by compaction, and the block counts as deduplicated. Document any new lock here before adding it.

## 10. Cleanup, integrity, and tools

- **Cleanup** (`POST /api/admin/gc`, and nightly at 02:00 local time): free unreferenced blocks as in §8, then compact containers below 50% live. Reports bytes reclaimed. "Live" means indexed (refcount-0 blocks in their grace period are kept and moved); the active container is never compacted; a container holding a live block that fails verification is not compacted. Moves for one container and its `CONTAINER_DROP` are one transaction; files are deleted after the checkpoint that follows the last move. A leftover file with nothing live (a crash after that checkpoint) is dropped by the next cleanup (D-048).
- **Integrity scan** (`POST /api/admin/scan`, nightly after cleanup): read every live block, verify CRC and SHA-256; failures are reported per block with the files affected. Progress events every 1%. The last result is kept in `data/scan_last`.
- **Verify on read:** downloads re-hash each block; on mismatch the download aborts and an `engine.corruption` event is emitted.
- **`--fsck`:** offline full check of I1–I3 by recomputing refcounts from all recipes and comparing, reading every referenced block, and checking every file's whole-file hash. Exit code 0 only if all pass.
- **`--rebuild-index`:** rebuild the index from container records and refcounts from recipes. Last-resort repair and a good viva talking point. The old index files are kept as `index/*.old`; the log is replayed for catalog records only; the first verified copy of each hash in container order wins; referenced blocks found nowhere are reported. Exit code 0 only if every referenced block was found (D-048).

## 11. Network layer

**HTTP/1.1, written from scratch** in `server/src/net/`:
- Blocking sockets through the platform layer (BSD sockets on Linux/macOS, Winsock 2 on Windows). Listening socket uses `SO_REUSEADDR` on POSIX and `SO_EXCLUSIVEADDRUSE` on Windows (where `SO_REUSEADDR` would let another process steal the port). Writes never raise SIGPIPE (`MSG_NOSIGNAL` on Linux, `SO_NOSIGPIPE` on macOS; Windows has no SIGPIPE). Accept thread pushes connections onto a bounded queue served by the worker pool. Keep-alive with a 5 s idle timeout and at most 100 requests per connection.
- Request line and headers capped at 16 KB total; bodies require `Content-Length` (reject `Transfer-Encoding: chunked` requests with 411). Body limits: 1 MB for JSON, 8 MB for binary upload endpoints.
- Responses use `Content-Length`; downloads stream from the engine through a writer callback with the length known from the file record.
- Static files from `web_root`: decode and normalize the path, reject `..` and NUL, serve correct MIME types, `Cache-Control: public, max-age=31536000, immutable` for hashed assets in `/assets/`, `no-cache` for `index.html`. Unknown non-API paths return `index.html` (client-side routing).
- Security headers on every response: `X-Content-Type-Options: nosniff`, `Referrer-Policy: no-referrer`, `X-Frame-Options: DENY`, and a CSP allowing only `'self'` plus `'wasm-unsafe-eval'` for scripts (needed by `hash-wasm`).

**WebSocket** at `/ws` (RFC 6455): validate `Upgrade`, `Connection`, version 13; `Sec-WebSocket-Accept` = base64(SHA-1(key + `258EAFA5-E914-47DA-95CA-C5AB0DC85B11`)). Require masked client frames. Server sends text frames with JSON events, answers ping with pong, handles close. Requires a valid session cookie. Each client has a bounded outgoing queue (drop the oldest `stats.updated` first if full).

## 12. HTTP API

All JSON uses camelCase. Errors: status code plus `{"error":{"code":"...","message":"..."}}`. Every mutating request must include header `X-NexusSync: 1` (CSRF guard together with the SameSite cookie).

**Health:** `GET /api/health` (no auth) → `{"status":"ok"|"recovering","version":"1.0.0","progress":{...}?}`

**Auth**
- `POST /api/auth/join` `{username, displayName, password, joinCode}` → 201 user; sets session cookie.
- `POST /api/auth/login` `{username, password}` → 200 user; sets `nx_session` cookie (`HttpOnly; SameSite=Strict; Path=/; Max-Age=43200`).
- `POST /api/auth/logout` → 204.
- `GET /api/me` → `{id, username, displayName, role}`.

**Files** (students see their own plus shared; admins see all)
- `GET /api/files?q=&sort=newest|name|size|shared&scope=mine|shared&cursor=&limit=50` → `{items:[File], nextCursor}`
- `GET /api/files/{id}` → File with `versions:[{id, version, size, createdAt}]` and `blocks:{total, uniqueToFile, shared}`
- `GET /api/files/{id}/content` → bytes, `Content-Disposition: attachment; filename*=UTF-8''...`
- `PATCH /api/files/{id}` `{shared: bool}` (owner or admin)
- `DELETE /api/files/{id}` (owner or admin) → 204

File object: `{id, name, version, prevVersionId, ownerId, ownerName, size, chunkCount, chunker, sha256, bytesSent, reusedPercent, createdAt, shared}`. `reusedPercent` = 100 × (size − bytesSent) / size, one decimal (100 for empty files). The list shows the head of each version chain (a file no other file names as `prevVersionId`), hides files flagged deleted, and its `cursor` is an opaque offset. A file the caller may not read answers 404, never 403, so ids reveal nothing. On a damaged block a download stops early (the connection closes) and `engine.corruption` is emitted.

**Uploads**
1. `POST /api/uploads` `{name, size, sha256, chunker:"cdc", chunkCount, versionOf?}` → `{uploadId, manifestPageSize: 8192, maxChunkBody: 8388608}`. If a file with the same name exists for this user, the new file becomes its next version unless `versionOf: null` is sent explicitly.
2. `POST /api/uploads/{id}/manifest?page=N` body `application/octet-stream`: up to 8192 entries of (`hash[32]`, `u32 len`) → `{need:[indices within the page]}`. Server records the declared hashes in the session (protects them from cleanup).
3. `POST /api/uploads/{id}/chunks` body: repeated (`u32 index`, `u32 len`, `data`). Server checks each `len` matches the manifest, hashes the data, compares to the manifest hash, rejects mismatches individually → `{accepted:[...], rejected:[{index, reason:"hash_mismatch"|"length_mismatch"|"unknown_index"}]}`. Clients send up to 3 chunk requests in parallel.
4. `POST /api/uploads/{id}/commit` → 201 File plus `{stats:{chunks, newChunks, bytesSent, reusedPercent}}`. Errors: 409 `missing_chunks` with indices (client sends them and commits again), 422 `file_hash_mismatch` (when `verify_on_commit` finds the rebuilt file differs from `sha256`), 507 `disk_full`.
5. `DELETE /api/uploads/{id}` cancels.
Sessions expire after `upload_session_ttl_s` of inactivity.

Validation and limits (DECISIONS D-029): names are 1–255 bytes of UTF-8 without control characters, `/` or `\`, and not `.` or `..`; files are at most 8 GiB; `chunkCount` must fit the size (fixed: exactly ⌈size/4096⌉; CDC: between ⌈size/16384⌉ and ⌊(size−1)/2048⌋+1); every manifest length must be possible for the chunker at that position (only the last block may be short), and one hash always has one length; a resent manifest page must be identical (409 `page_changed`); at most 8 open upload sessions per user (429) and 4,194,304 declared chunks over all sessions (503). `versionOf` absent: next version of the user's newest file with that name; `null`: a new file; an id: that file (the user's own). Upload ids are 32 random hex characters; another user's upload id answers 404. `bytesSent` counts the bytes of accepted chunks, including those an earlier session of the same owner sent for the same file (same SHA-256) before it was cancelled, expired, failed, or abandoned: a resumed upload reports every byte that crossed the network (D-041; kept in memory, so a restart in between forgets them). A retried upload of the same file finds most blocks present, so resuming costs little.

**Compare:** `GET /api/compare?a={fileId}&b={fileId}&mode=fixed|cdc&from=0&count=32` → `{mode, files:{a:{id,name,version,size}, b:{...}}, totals:{a:{chunks,bytes}, b:{chunks,bytes, reused, sentBytes}}, firstDifference, window:{from, count, a:[Block], b:[Block]}, minimap:[[1|0, run], ...]}` (minimap over b's blocks, 1 = also in a). Without `from` the window starts 3 blocks before the first difference; `count` is 1–64; a file the caller cannot read answers 404; files over 1 GiB answer 422 `too_large_to_compare` (D-043). `reused` means "the other version has a block with this hash"; `sentBytes` counts each distinct new block of b once. Each side comes from its recipe when it was stored with the requested chunker (then `stored` is filled; `sharedBy` is the block's refcount), otherwise from re-chunking the file read through the engine (`stored` null). Block: `{index, size, sha256, reused, matchIndex|null, stored:{containerId, offset, sharedBy}|null}`. For `mode=cdc` with CDC-stored files, results come from recipes directly. For `fixed`, the server rebuilds both files in memory streams, chunks them at 4 KB, and computes matches by hash; `stored` is null. Cache results per (a, b, mode) for 5 min.

**Stats:** `GET /api/stats` → `{logicalBytes, physicalBytes, ratio, writesSkippedBytes, networkSavedBytes, files, users, uploadsThisWeek, wal:{lsn, recordsSinceCheckpoint, sizeBytes, lastCheckpointAt, lastCheckpointMs}, index:{entries, globalDepth, buckets, avgFillPercent, bufferHitRate, dirtyPages, bufferHits, bufferMisses}, containers:{count, bytes, reclaimableBytes, liveHistogram:[10 buckets]}, integrity:{lastScanAt, verified, failures}, cleanup:{pendingBytes, nextRunAt}, uptimeS}`. Students receive the global storage numbers plus `mine:{files, bytes, reusedPercent}`; engine internals (`wal`, `index`, `containers`, `integrity`, `cleanup`) are admin-only. `files` counts files not flagged deleted; `uploadsThisWeek` counts those created in the last 7 days. Times not known yet are `null` (`lastCheckpointAt` before the first checkpoint since start, `integrity.lastScanAt` before the first scan, `cleanup.lastRunAt` before the first cleanup since start); `bufferHitRate` is a percentage since start.

**Block map:** `GET /api/blockmap?tiles=384` → `{tileBytes, physicalBytes, blocks, tiles:[level 0–4 per tile]}`; `GET /api/blockmap?tiles=384&file={id}` → `{fileId, tileBytes, tiles:[indices containing the file's blocks]}` (404 for a file the caller may not read). `tiles` is 1–4096 (400 otherwise). Tiles are equal slices of the stored data bytes in storage order (container, offset); a tile's level is the byte-weighted mean share count, rounded: 1 below 1.5, 2 below 2.5, 3 below 4.5, else 4; 0 is empty (D-040). Computed by walking the index in storage order, cached and recomputed at most every 2 s after changes (a file query that finds blocks newer than the cache rebuilds at once).

**Admin** (role admin): `GET /api/admin/users`, `POST /api/admin/users/{id}` `{role?, status?}`, `GET /api/admin/join-code`, `POST /api/admin/join-code/rotate`, `POST /api/admin/checkpoint`, `POST /api/admin/gc`, `POST /api/admin/scan`, `GET /api/admin/events?limit=200`, `GET /api/admin/recoveries`, `GET /api/admin/addresses` (LAN IPv4 addresses with port, for the connect QR codes).
Shapes (M11, D-046):
- `users` → `{items:[{id, username, displayName, role, status:"active"|"disabled", files, bytes, createdAt, lastSeenAt|null}]}`.
- `users/{id}` takes `{role?:"admin"|"student", status?:"active"|"disabled"}` and returns the same object. An admin cannot change themself (409 `cannot_change_self`); disabling ends the user's sessions and sockets.
- `events?limit=1..256` → `{items:[{type, at, data}]}`, oldest first, the last 256 events except `stats.updated` and `upload.progress`.
- `recoveries` → `{items:[{at, recordsReplayed, transactionsRedone, transactionsDiscarded, pagesRepaired, walBytesCut, durationMs, result:"clean"|"recovered"}]}`, newest first, from `data/recoveries`.
- `addresses` → `{port, listen, items:[{address, url}]}`.
- `gc` and `scan` (M12, D-048) start a maintenance job and answer 202 `{started:"gc"|"scan"}`; while a job runs both answer 409 `maintenance_running`. Progress and results arrive as events (admins only):
  - `engine.gc` `{phase:"freeing"}`, `{phase:"compacting", container, done, total}`, then `{phase:"done", trigger:"admin"|"nightly", blocksFreed, bytesFreed, containersCompacted, blocksMoved, bytesMoved, bytesReclaimed, containersSkipped, durationMs}` or `{phase:"failed"}`.
  - `engine.scan` `{phase:"scanning", done, total, percent}` at most once per 1%, then `{phase:"done", trigger, verified, failures, durationMs, items:[{sha256, containerId, offset, reason:"crc"|"hash"|"unreadable", files:[{id, name}]}]}` (at most 100 items) or `{phase:"failed"}`.
  - `engine.corruption` `{sha256, containerId, offset, reason, files}` once per damaged block a scan finds.
  - In `/api/stats`, `cleanup` is `{pendingBytes, nextRunAt, running, lastRunAt|null, lastReclaimedBytes}` and `integrity` is `{lastScanAt|null, verified, failures, running, percent}`.
- For admins, `/api/stats` also carries `server:{version, dataDir, chunker, cdcMin, cdcAvg, cdcMax, fixedSize, containerSize, checkpointIntervalS, gcGraceS}`.
- `/api/health` while recovering adds `progress.recordsReplayed`.

**WebSocket events** `{type, at, data}`: `stats.updated` (throttled to 2/s), `upload.started`, `upload.progress` `{uploadId, fileName, phase, done, total, newTiles:[...]}`, `upload.committed`, `upload.failed`, `file.deleted`, `file.updated`, `engine.checkpoint`, `engine.gc`, `engine.scan`, `engine.corruption`, `engine.recovery`, `server.shutdown`. Non-admins receive events for their own and shared files; for other users' uploads they receive only aggregate `stats.updated` (the global storage numbers, at most every 500 ms and only after a change) and an anonymous `upload.progress` copy `{uploadId:null, phase, newTiles}`. `newTiles` are the tiles (of 384) holding the blocks a chunk request just stored.

## 13. Authentication and security

- Passwords: PBKDF2-HMAC-SHA256 (in-repo, tested against RFC 7914 §11 and RFC 6070-style vectors), 600,000 iterations, 16-byte random salt, 32-byte output; constant-time comparison (`nx_ct_equal`). Minimum length 10. The iteration count is stored per user so it can be raised later.
- Secure random bytes: `plat_random_bytes()` uses `getrandom()` on Linux, `arc4random_buf()` on macOS, and `BCryptGenRandom()` on Windows. A failure is fatal; never fall back to `rand()`.
- Sessions: 32 random bytes from `plat_random_bytes`, hex-encoded in the cookie, stored as SHA-256(token) → {user, expiry} in memory and in `data/sessions` ("NXSS", u16 version, u16 0, u32 count, count × (hash[32], u32 user, u64 expires_ms), u32 crc32), rewritten atomically and fsynced before a login answers and after a logout. A crash or restart therefore keeps everyone signed in (DEMO step 5:45, DECISIONS D-030). The token itself is never stored.
- Join code: 8 characters from `23456789ABCDEFGHJKMNPQRSTUVWXYZ` (no 0/O, 1/I/L), compared case-insensitively in constant time, set at `--init` (random, or `join_code` from the config), stored in `data/join_code`, rotatable by admins. First admin created with `--create-admin` on the server terminal.
- Usernames: 3–32 characters of `[a-z0-9._-]`, starting with a letter or digit, stored lower-case (input is lower-cased). Display names: 1–64 bytes of valid UTF-8 without control characters or edge spaces. Passwords: 10–256 bytes.
- Login responses do not reveal whether a username exists: unknown names cost the same PBKDF2 work and get the same 401; `account_disabled` (403) is only reported after a correct password. Wrong join codes count as failures for the IP rate limit.
- Rate limits: 5 failed logins per username per minute and 20 per IP per minute → 429.
- Authorization checked in every handler; file access requires owner, shared flag (read only), or admin.
- Never trust client-supplied hashes: every received block is hashed by the server. Never trust lengths or indices: all bounds-checked.
- No secrets in the repo, logs, or URLs. Session cookie not marked `Secure` because the LAN uses HTTP; documented limitation.
- Known side channel: a client can infer a block exists from the `need` list. Documented; proof-of-ownership challenges are a stretch goal.

## 14. Build order

Built in one autonomous run, in this order. Each step ends only when its checks pass under ASan and have been executed, not just written. Progress is tracked in `docs/PROGRESS.md` (see `CLAUDE.md`). The engine comes first because everything on screen depends on it being correct.

| # | Step | Acceptance checks |
|---|---|---|
| M1 | Scaffold | Repo layout, platform layer, Makefile targets (`all`, `debug`, `asan`, `tsan`, `test`) on Linux and macOS and (`all`, `test`) on Windows, test runner, `nexusd --version`; Vite app builds; `scripts/check.py` runs both test suites; CI workflow for the three OSes; PROGRESS, DECISIONS, LEARNING created |
| M2 | Chunkers and hashing | Fixed and CDC chunkers, gear generator, SHA wrapper; fixture cut points stable; TS chunker passes parity tests |
| M3 | Containers, buffer pool, index | Insert and look up 1M random hashes with splits and directory doubling; Clock eviction tests; property test against an in-memory map |
| M4 | Catalog, recipes, engine API | In-process put/get/delete round-trip of 200 random files including duplicates and edited copies; I1–I3 checked by `engine_fsck` |
| M5 | WAL, checkpoint, recovery | Torn-tail, torn-checkpoint (doublewrite), and crash-between-steps tests; in-process crash loop of 20 iterations passes |
| M6 | HTTP, WebSocket, static, auth | Parser tests including malformed and oversized input; join, login, logout, rate limit; WS handshake and events; serves the built web app |
| M7 | Upload, download, concurrency | Manifest, chunks, commit, cancel, resume; 8 concurrent CLI clients uploading overlapping files, then `--fsck` clean; downloads match SHA-256 |
| M8 | Frontend foundation | Tokens, self-hosted fonts, shell, nav, status indicator, sign-in and join, API client, WS client with reconnect; screenshots checked against DESIGN |
| M9 | Overview, Files, upload tray | Real data everywhere; block field, linked hover, live tile pulses; files list, drawer, versions, delete with undo; full upload flow with all phases and failures |
| M10 | Compare versions | Pickers, fixed and CDC modes, window, minimap, hover partners, details, animations; matches DESIGN §8.5 |
| M11 | Engine and Users screens, system states | All Engine sections with live data and actions; users admin; offline, recovering, session-ended, empty, loading states |
| M12 | Cleanup, compaction, integrity | Cleanup respects grace period and open sessions; compaction survives a crash mid-way; scan detects an injected corrupt byte; nightly schedule |
| M13 | CLI, demo tooling, evaluation tooling | `nexus` CLI; `crash_test.py 50` passes against the real server; `make_demo_data.py`, `demo_reset.py`; `run_eval.py` produces tables and charts when pointed at a dataset folder (run it on the generated demo data as a smoke test) |
| M14 | Polish and final report | Polish pass across the app; every screen and state screenshotted at three sizes and checked per DESIGN §13; Playwright end-to-end passes against a real server; final report in PROGRESS |

**Done by humans, not in the run:** the Phase 1 proposal (problem, measured duplication in real lab data, architecture diagram, mockups); collecting real lab datasets and running the evaluation on them; testing on real laptops and the real network per `docs/DEMO.md`; reviewing the code and studying `docs/LEARNING.md` for the viva.

**What to show at each phase:** Phase 1, the proposal package above. Phase 2, the working system: identical upload sends nothing, edited CSV sends little, `kill -9` and recovery with `--fsck`, download hash check. Phase 3, the full demo script in `docs/DEMO.md` with evaluation results on real data.

## 15. Testing strategy

- **Unit (C):** chunkers, SHA, CRC32, page and record serialization round-trips, extendible hashing (splits, doubling, many collisions in top bits), buffer pool (pin, evict, dirty rules), WAL encode/decode, torn-record detection, superblock A/B selection.
- **Property tests (C):** random sequences of put, delete, checkpoint, and simulated crash against a simple in-memory model; compare all observable state.
- **Integration (C + HTTP):** spin up `nexusd` on a temp dir; drive the API with the CLI and a small C test client; malformed requests; auth rules; upload resume; 409 and 422 paths.
- **Concurrency:** 8–16 clients uploading overlapping and identical files at once; run under ThreadSanitizer in the separate `make tsan` target before the final report.
- **Platforms:** CI (`.github/workflows/ci.yml`) builds and runs `make test` on ubuntu-24.04 (with ASan/UBSan and TSan), macos-latest (with ASan/UBSan), and windows-latest under MSYS2 UCRT64 (no sanitizers). The crash test runs on all three.
- **Crash:** `scripts/crash_test.py N`: start server, start random uploads and deletes, hard-kill it (`Popen.kill()`: SIGKILL on POSIX, TerminateProcess on Windows) at a random moment between 0.1 and 3 s, restart, `--fsck` must pass, every committed file must download with a matching hash. Repeat N times.
- **Corruption:** flip a byte in a container; scan and verify-on-read must detect it and name affected files.
- **Frontend:** Vitest for the chunker parity, formatting helpers, and state reducers; Playwright for join → upload → second identical upload shows "nothing sent" → download → compare → delete with undo. Run Playwright against a real `nexusd`, not mocks.
- **Design QA:** Impeccable critique and audit per screen (CLAUDE.md).

## 16. Evaluation plan

Datasets (collected with permission, stored in `eval/datasets/`, never committed): lab PDFs and manuals; assignment submissions; datasets (CSV); source code trees as tar; VM or ISO images; edited versions of text and CSV files; zip and docx files to show the limitation.

Baselines: plain storage (sum of sizes), gzip per file, whole-file deduplication (SHA-256 of whole file), fixed 4 KB block dedup, NexusSync CDC dedup.

Metrics: deduplication ratio and bytes saved per category; network bytes sent for second and edited uploads; upload and download throughput (MB/s) on the LAN and on localhost; index lookups per second; checkpoint duration; recovery time versus WAL size; average and distribution of CDC chunk sizes; CPU time spent hashing and chunking.

Output: `eval/results/*.csv` and charts (matplotlib, PNG and SVG), plus a summary table for the report. State hardware, dataset sizes, and how many runs were averaged.

## 17. CLI `nexus`

```
nexus --server http://192.168.1.20:8080 login <username>
nexus put <file> [--fixed] [--new-file]
nexus get <id|name> [-o out]
nexus ls [--shared]
nexus rm <id>
nexus stats
```
Session token stored in `~/.nexussync/session` with mode 0600 (`NEXUS_HOME` overrides the directory). The password is asked on the terminal; scripts may set `NEXUS_PASSWORD`. Output is plain text; `--json` for scripts. `get` writes to `<name>.part`, checks the SHA-256, then renames; a mismatch deletes the partial file. Built by `make -C server` as `server/build/nexus`.

## 18. Stretch goals (only after M14 is done)

Group commit; `epoll`-based networking; B+ tree index over file names; mDNS name `nexussync.local`; proof-of-ownership challenges for declared blocks; HTTP range downloads; compressing unique blocks with zstd after deduplication; recipe file compaction; a second server as a read-only replica.
