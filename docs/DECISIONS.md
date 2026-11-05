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

## D-013 Page LSNs and "pool full → checkpoint" (M3, used in M5)

**Problem.**
- The spec's buffer pool is no-steal: dirty pages reach disk only at a checkpoint.
- One file commit can touch more index pages than the pool holds. A 300 MB CSV is about 64k blocks, so its commit applies about 64k `REF_INC`s spread over every bucket page. With the default 4,096 frames, the pool fills before the commit has been applied.
- Blocking forever, or raising the pool size until it fits, would make the server fail exactly when the store grows.

**Chosen.** Two classic DBMS techniques:
1. **Page LSN (as in ARIES).**
   - Every page stores the LSN of the last WAL record applied to it.
   - During redo, a change is skipped when the page's LSN is already at or beyond the record's LSN (`NX_EXT_SKIPPED`). That makes replaying a record idempotent.
   - A split keeps the old page's LSN on *both* pages, because a split is structural and contains no logged change. (Corrected in M4, see D-018: the M3 code copied the LSN to the new page but then stamped both pages with the current record's LSN.)
2. **The pool never blocks.** When no clean, unpinned frame is free, `pin` returns `NX_BP_FULL`. The caller runs a checkpoint and retries.

**Consequence for M5.**
- A checkpoint may run while a committed transaction is only partly applied to pages. In that case the checkpoint LSN must be set to just before that transaction's `BEGIN`, so recovery replays it, and the page LSNs make the replay skip whatever was already applied.
- This also requires committed transactions to be applied to pages in LSN order: one applier at a time (the commit pipeline in M5). Otherwise a page could show a later LSN while missing an earlier change.

**Alternatives rejected.**
- *Steal with undo logging.* Needs undo records and compensation: much more machinery.
- *A pool that always fits the whole index.* Memory grows with stored data until the server cannot start.

**Tested in M3.**
- 1M inserts through a 4,096-frame pool (372 checkpoints).
- 200k random operations through a 64-frame pool (1,708 checkpoints), matching an in-memory model.
- The redo skip rule, in a dedicated test.

## D-014 Which container is active, and torn container tails (M3)

**Active container.** The active container is the one with the highest id among the files on disk, not a value read from the superblock. A new container is created (and its directory entry fsynced) before any record is written to it, so the highest id on disk is always the newest container. Restart after a crash therefore never depends on a superblock written before that crash.

**Torn tail.** A crash during an append can leave a partial record at the end of the active container. Appending after it would put valid records behind unparsable bytes, so a sequential scan (`--rebuild-index`, compaction) could not find them. Instead, on startup:
- the damaged container is sealed as it is, with no truncation, so we never delete bytes we cannot classify;
- appends continue in a new container;
- the damaged bytes are orphan bytes, reported by the scan and reclaimed by compaction.

**Half-created container.** If the highest container has no valid header, it was created but its header never became durable. No committed record can be inside it (the record fsync would also have made the header durable), so it is rewritten.

**Read handles.** Reads go through a cache of 64 open handles with reference counts. macOS allows only 256 open files per process by default, and a large store has thousands of containers.

## D-015 Small format additions in M4 (spec 1.2)

**Question.** SPEC §4 says every format starts with a magic and a version, but the §5 recipe format described only records, and the superblock and doublewrite formats gave fields without byte positions.

**Chosen.**
- `recipes.dat` gets a 16-byte file header (`NXRF`, version 1). A recipe file without a durable header cannot hold a committed record (the record's fsync would have made the header durable too), so a file shorter than 16 bytes is rewritten on open.
- The superblock byte offsets and the doublewrite layout are now written in SPEC §5.

**Alternatives rejected.** A version field inside every recipe record: 2 bytes per file for no benefit, because all records of one file share one format.

## D-016 One transaction mutex instead of a checkpoint rwlock plus 256 index stripes (M4)

**Problem.** Page-LSN redo (D-013) is only correct if committed transactions are applied to pages in LSN order. With the v1.1 lock plan (writers share the checkpoint rwlock, and lock only the index stripes they touch), two commits could apply in parallel. A page could then show LSN 120 while missing the change of LSN 118, and recovery would skip 118.

**Options.**
1. *One transaction mutex* held while assigning LSNs, appending to the WAL, fsyncing, and applying. Checkpoints take it too.
2. *Keep stripes, order applies afterwards:* commits log in parallel but apply strictly in LSN order through a sequencer queue.
3. *Per-record LSNs inside each index entry* instead of per page.

**Chosen: 1.** The work under the mutex is short: one WAL fsync and in-memory page changes. Everything slow happens outside it: hashing, container appends and their fsync, recipe appends, whole-file verification, and all reads. With one applier, stripes protect nothing, so they are removed (SPEC §9 v1.2). If commit throughput ever matters, group commit (a stretch goal) batches the fsync of several waiting transactions under the same mutex.

**Consequences.**
- Two uploads storing the same new block both append it to a container. The second `CHUNK_ADD` finds the entry and is a no-op; its copy is orphan bytes for compaction. The upload is told the block was deduplicated.
- A transaction whose apply fails after the commit point leaves pages and log disagreeing. The engine then refuses further writes until restart, where recovery (M5) replays the transaction.

## D-017 Meaning of the "deleted" flag (M4)

**Question.** The files record has a `deleted` flag (bit 1), and there is also a `FILE_DEL` record. What does each mean?

**Chosen.**
- `FILE_UPDATE` setting the deleted flag is a *soft delete*: the file is hidden in the UI (delete with undo, DESIGN), but it keeps its recipe references, so its blocks stay alive.
- `FILE_DEL` is the real delete: the record is removed and each block's refcount is decremented.
- I2 in fsck therefore counts references from every catalog record, flagged or not.

**Alternative rejected.** Decrementing refcounts at soft delete and incrementing them again on undo: two transactions instead of one, and an undo could fail if cleanup freed a block in between.

## D-018 A bucket split keeps the old page LSN (M4 bug fix in M3 code)

**Bug found while building the engine.** `nx_exthash_split` stamped both pages with the LSN of the record being applied. During redo, the record that needed the split is retried after the split; it then saw `page_lsn >= its LSN` and was skipped as "already applied". The index entry was silently lost. M3's tests never redid an insert across a split, so they passed.

**Fix.** A split is structural and is not logged; both pages keep the old page's LSN. The split no longer takes an LSN argument. New test `test_redo_insert_across_split` fails on the old code and passes now.

## D-019 Catalog lookups by name are linear scans (M4)

**Question.** SPEC §7.4 lists in-memory indexes for users by username, files by owner, and files by (owner, name). Hash maps or linear scans?

**Chosen.** Files by id use a hash map (every download and delete needs it). Lookups by username, by owner, and by (owner, name) scan the in-memory array. A lab has hundreds of users and a few thousand files; scanning 10,000 decoded records takes microseconds and the code stays short.

**When to change.** If a store grows past about 100,000 files, add hash maps keyed by username and by (owner, name). The function signatures in `catalog.h` already hide how lookups work.

## D-020 Crash tests in process, with named crash points (M5)

**Question.** M5 needs "crash between steps" tests and a 20-iteration crash loop. How do we crash at an exact step, on every OS?

**Options.**
1. *Named crash points* (`util/failpoint.h`). Code that writes durable state calls `nx_fail_hit("name")` between steps. A test arms one name; when it fires, that write and every later one is skipped, and the test abandons the engine without a checkpoint (`nx_engine_abandon`) and reopens it. "Torn" points first write half of the buffer.
2. *fork() a child and kill -9 it* at a random time.
3. *Only the real-process crash test* (`scripts/crash_test.py`, M13).

**Chosen: 1 here, and 3 later as well.** Crash points hit exact steps (between the WAL fsync and the apply, between the doublewrite and the in-place writes...), so each test can state the one correct outcome. `fork()` does not exist on Windows, and random timing rarely lands between two particular steps. The real-process test in M13 covers what this cannot: an actual SIGKILL/TerminateProcess.

**What it models.** A process crash (kill -9): data already handed to the OS stays, nothing after the crash point is written. It does not model power loss, where writes not yet fsynced may vanish; that depends on the drive honouring flushes (SPEC §8).

**Cost when not testing.** One relaxed atomic load per crash point.

## D-021 WAL cut rules after a crash (M5)

- **Where a torn record can be.** Only at the end of the newest segment, because a segment is fsynced before the next one starts and a transaction is never split across segments. A bad record anywhere else means the log was damaged, and recovery refuses to open instead of truncating committed work away.
- **What is cut.** Everything after the last `COMMIT` or `CHECKPOINT` record. That removes a torn tail and also a complete transaction that never got its `COMMIT`. Their LSNs are handed out again; this is safe because nothing from an uncommitted transaction ever reaches a page (pages change only after the commit fsync).
- **Next LSN.** Never at or below the superblock's `checkpoint_lsn`, because pages may carry LSNs up to it and redo compares against them.
- **`CHECKPOINT` records are not fsynced.** They only mark a safe end of the log. Recovery always starts from the superblock's `checkpoint_lsn`, never from a `CHECKPOINT` record, so losing one costs nothing.

## D-022 Checkpoint LSN while a transaction is half applied (M5)

D-013 said a checkpoint taken in the middle of applying a transaction uses "the LSN just before its `BEGIN`". The engine uses `stable_lsn`, the `COMMIT` LSN of the last fully applied transaction. That can be slightly lower (a `CHECKPOINT` record may sit between the two), which only means recovery replays a little more. Replay is idempotent, so lower is always safe; higher would not be.

## D-023 cJSON: vendored as published, parser only (M6)

cJSON v1.7.18 (MIT, `server/vendor/cJSON.c`, `cJSON.h`, `cJSON.LICENSE`) was downloaded from the official GitHub repository with the team's approval. Its `#include`s were checked (C standard headers only). It is compiled **without our warning flags** (`-w`), because we do not edit third-party code and its printer uses `sprintf`, which macOS marks deprecated. It is still compiled under ASan/UBSan/TSan with everything else. We use only the parser; every JSON byte we send is written by `util/json_out.c`.

## D-024 WebSocket clients leave the worker pool (M6)

**Problem.** SPEC §2 has a fixed pool of 8 workers, each serving one connection at a time. A WebSocket stays open for as long as a tab is open, so 8 open tabs would take every worker and the server would stop answering HTTP.

**Chosen.** After the `101` response, the socket is handed to the hub (`net/ws.c`):
- one small reader thread per client (answers ping, handles close, notices a dead peer after 60 s of silence);
- one broadcaster thread, the **only** writer to every WebSocket, so frames from two threads can never interleave on one socket;
- a ticker thread wakes the broadcaster every 250 ms (pings every 25 s). It uses `plat_sleep_ms` because the platform layer has no timed condition wait, and adding one means Windows code we cannot compile here.

At most 64 WebSocket clients (`max_connections`). Closing: the reader queues the close frame and keeps reading; the broadcaster sends it and then shuts the socket down. (The first version shut down in the reader and lost the close frame in a race; the integration test caught it.)

**Alternative rejected.** One thread multiplexing all sockets with `poll()`/`WSAPoll`: fewer threads, but a new platform API and a harder viva explanation, for at most 64 clients.

## D-025 Where the join code lives (M6)

SPEC §3 lists `join_code` as a config key "random on `--init`", and §13 makes it rotatable by admins. A rotation must survive restarts, and rewriting the admin's hand-edited config file is fragile. So the current code lives in `data/join_code` (replaced atomically: write a temp file, fsync, rename, fsync the directory). The config key only seeds `--init`.

## D-026 Static files are served while recovering (M6)

SPEC §8 says every endpoint other than `/api/health` returns 503 while recovering. We apply that to `/api/*` and `/ws`, but still serve the web app's static files: otherwise a browser opened during recovery shows its own error page instead of NexusSync's "recovering" state (DESIGN, M11). Static files do not touch the engine.

## D-027 Test-only PBKDF2 iteration count (M6)

Real logins use 600,000 iterations (SPEC §13), about 0.2 s each. The integration tests sign in hundreds of times, so they set `app->pbkdf2_iters = 1000` **in code**. There is deliberately no config key for it, so a deployment cannot weaken password hashing by accident. The count is stored per user, so accounts created by tests simply carry their own count.

## D-028 What failed logins reveal (M6)

- An unknown username still costs one full PBKDF2 run and returns the same 401 as a wrong password, so timing does not reveal which usernames exist.
- `account_disabled` is only reported after the correct password.
- Rate limits are checked *before* hashing, so a flood of guesses cannot burn the server's CPU.
- Known limit: `/api/auth/join` answers `username_taken`, which reveals that a name exists to anyone holding the join code. Acceptable inside one lab.

## D-029 Upload limits (M7)

Upload sessions live in memory: per declared chunk, 36 bytes of manifest plus a flag and a hash-map slot. Without limits, one client could declare a billion chunks and exhaust the server's memory. Chosen limits:
- files up to **8 GiB** (a 4 GB ISO, the largest demo file, fits with room to spare);
- **8 open sessions per user** (the browser uploads a few files at a time);
- **4,194,304 declared chunks** across all sessions, about 150 MB of manifests in the worst case. A typical 4 GB file has about 800,000 CDC blocks.

The chunk count must also fit the size and chunker, so a client cannot declare 4 million chunks for a 10-byte file. Raising the limits later is a constant change in `api/uploads.h`.

## D-030 Sessions survive restarts (M7, changes SPEC §13)

**Conflict.** SPEC §13 said a restart signs everyone out. DEMO.md's crash step (5:45–7:30) expects the clients to show "retrying", then carry on, and the interrupted upload to resume, without signing in again. With in-memory sessions every laptop would land on the sign-in screen during the most important part of the demo. The crash test hit this at once.

**Chosen.** Keep SHA-256(token) → {user, expiry} in `data/sessions`. The whole table is rewritten atomically (temp file, fsync, rename, fsync directory) **before** a login answers and after a logout or account change. Then a session never outlives its logout, and a login never answers with a session that a crash would lose.

**Why this is safe.** The file holds hashes of tokens, never the tokens (a unit test checks the token bytes do not appear in it), and it is created with mode 0600. A damaged file means everyone signs in again, nothing worse.

**Cost.** One small fsync per login and logout, which is negligible at lab scale.

**Alternative rejected.** Sessions as WAL records in the catalog: durable for free, but every login would become an engine transaction, and session state would mix with file metadata for no gain.

## D-031 The CLI arrives in M7 (M7)

M7's acceptance needs "8 concurrent CLI clients", so a working `nexus` (`login`, `put`, `get`, `ls`, `rm`) is built now; `stats` and the polish come in M13.
- Build: `make -C server` also builds `server/build/nexus` from `cli/`, linking the same chunker, crypto, platform, and cJSON code as the server. That guarantees the CLI chunks byte for byte like the server.
- `NEXUS_HOME` and `NEXUS_PASSWORD` exist for scripts (crash and concurrency tests, demo seeding). A password in an environment variable is visible to the same user's processes; the interactive prompt remains the normal path.
- `put` reads the file twice (chunk and hash, then send only the needed blocks), so memory stays at about 44 bytes per block, whatever the file size.

## D-032 Real-process tests: concurrency_test.py and crash_test.py (M7)

- `scripts/concurrency_test.py` runs 8 CLI processes (8 users) uploading overlapping files at the same moment, then verifies every download's SHA-256, the listings, and `--fsck`. It is part of `scripts/check.py` (about 10 s).
- `scripts/crash_test.py N` hard-kills the real server N times during random uploads and deletes from several CLI processes.
  - After each kill: restart, then check that exactly the committed files exist, each with its SHA-256.
  - An operation in flight at the kill must have happened completely or not at all (checked by its unique name).
  - After a clean stop, `--fsck` must pass.
- CI runs both on Linux, macOS, and Windows (10 crash rounds; the full 50 before a demo).

## D-033 Data race in index statistics (M7 fix in M3 code)

ThreadSanitizer, running the M7 upload tests, found `nx_exthash_get_stats` reading the directory depth and page count without the directory lock while a split changed them. Before M6 nothing read statistics concurrently. The server's background thread now does so every 250 ms. Fix: the stats functions take the shared directory lock. All callers were checked: none holds the lock already.

