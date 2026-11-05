# NexusSync — progress

Checklist of every build step in `docs/SPEC.md` §14. A box is ticked only after the check was actually executed and passed.
Resume after an interruption: read `CLAUDE.md`, then this file, re-run the tests of the last ticked step, continue from the first unticked item.

Platforms: macOS (verified locally on Apple Silicon), Linux and Windows (verified in CI once the repo is pushed; see "Open problems").

Current scope agreed with the team: M1–M7 in this run; M8–M14 in later sessions.

**Status (2026-11-05): M1–M5 done and committed. Next: M6.** See "Handoff notes for M6" below.

## M1 Scaffold
- [x] Repo layout per `CLAUDE.md` (plus `server/src/platform/`); module folders fill in from M2
- [x] Makefile targets `all`, `debug`, `asan`, `tsan`, `test`: all run and pass on macOS (Apple clang 21, arm64), also with `WERROR=1`. Linux and Windows: see CI item
- [x] C test runner (`server/tests/nx_test.h`)
- [x] `nexusd --version` prints `nexusd 0.1.0 (spec 1.1, macos)`
- [x] Platform layer: POSIX implemented, 11/11 tests pass under ASan+UBSan and TSan on macOS. Win32 implemented but **not yet compiled** (no Windows here)
- [x] Vite app builds (`npm run build`), vitest 4/4, `npm audit` 0 vulnerabilities
- [x] `scripts/check.py` runs both test suites (verified on macOS)
- [ ] CI workflow for ubuntu / macos / windows: written (`.github/workflows/ci.yml`), **not run yet**: needs a GitHub repo
- [x] PROGRESS, DECISIONS (D-001..D-009), LEARNING (M1) created; SPEC/CLAUDE/DEMO/PROMPT updated to v1.1 for cross-platform

## M2 Chunkers and hashing
- [x] Fixed and CDC chunkers + streaming chunker; any piece size (1 byte to 1 MB) gives the same blocks as whole-buffer chunking
- [x] Gear generator writes C and TS tables from one seed; `--check` added to CI
- [x] SHA-256, SHA-1, HMAC-SHA256, PBKDF2, CRC-32 pass NIST/RFC vectors and 352 Python-reference vectors; mutation check fails as expected; PBKDF2 600k = 230 ms (ASan)
- [x] Fixture cut points stable (10 fixtures, `chunks_expected.txt` from the C code); 300-byte insert: CDC reuses 2169/2170 blocks, fixed 1/2561; average CDC block 4.9 KB
- [x] TS chunker + hash-wasm match the C reference on all 10 fixtures in 8 MB and odd-sized windows (vitest 16/16); mutation check fails as expected

## M3 Containers, buffer pool, index
- [x] Containers append, seal, read back with CRC check; reopen, torn tail (sealed, new container), half-created header, corruption detection, 4-thread appends (7/7)
- [x] Insert and look up 1M random hashes with splits and directory doubling (depth 15, 16,641 buckets, 77% fill, 372 no-steal checkpoints); reopen from dir.bin; 600 hashes sharing 16 top bits
- [x] Clock eviction tests: exact second-chance order, pinned frames kept, no-steal refuses to write dirty pages (NX_BP_FULL), CRC detects a flipped bit (6/6)
- [x] Property test: 200k random insert/lookup/ref/move/remove ops through a 64-frame pool match an in-memory model; 4-thread insert+lookup test; full suite clean under ASan+UBSan and TSan

## M4 Catalog, recipes, engine API
- [x] Slotted-page catalog (users, files): round trips, 3,000 files with deletes and hole reuse, reopen, idempotent apply, 20k random ops through an 8-frame pool against a model (6/6)
- [x] Recipes append + read: reopen, CRC/id/count/length mismatches refused, torn tail harmless; plus superblock A/B fallback, doublewrite round trip and torn batches, WAL payloads (4/4)
- [x] Round-trip of 200 random files including duplicates and edited copies: byte-exact reads, 30 duplicates sent 0 bytes, edited CDC copies sent 11.2%, versions linked, 57 deletes, reopen, delete-all; 48-frame pool forced 125 checkpoints (many mid-transaction)
- [x] I1–I3 checked by `engine_fsck`: clean at every stage above; injected damage detected for each (flipped container byte → I1, bumped refcount → I2, wrong whole-file hash → I3)
- [x] Error paths: missing chunks (409 list), bad block/file hash (422), malformed commits; users add/duplicate/update/persist
- [x] 6 concurrent upload threads with overlapping content through a 16-frame pool (40+ checkpoints): fsck clean, ThreadSanitizer clean (3 runs)
- [x] Full suite: `python3 scripts/check.py` passes (61 C tests in 9 executables under ASan+UBSan, web 16/16), `-Werror` clean in asan and release builds
- [x] Bug fixed in M3 code: index split stamped pages with the current record's LSN, so redo lost the insert that caused a split (D-018, regression test added)

## M5 WAL, checkpoint, recovery
- [x] WAL encode/decode, torn-tail truncation: log cut at each of 294 byte positions of the last transaction always recovers to the previous commit and appends continue; uncommitted complete transaction removed; 12-segment rollover, deletion below a checkpoint, reading from a checkpoint LSN; damage in an older segment refused (5/5)
- [x] Checkpoint with doublewrite; torn-checkpoint repair test: torn page (first and 4th page of the batch), crash after staging, before superblock, torn superblock (falls back to the older copy and replays 8 transactions onto newer pages), before clearing; all repaired, fsck clean. Without `doublewrite.bin` a torn page stops the store from opening (bucket page checksum mismatch)
- [x] Crash-between-steps tests (named crash points, D-020): 19 cases across upload, commit, delete, checkpoint, each with its exact expected outcome, fsck clean, store keeps working afterwards
- [x] In-process crash loop of 20 iterations: 315 random operations, 14 crashes inside an operation, 102 transactions redone, 17 pages repaired; recovered state matched the committed-state model and fsck was clean every time
- [x] Committed work survives a kill with no checkpoint since open (22 transactions replayed); ids continue past replayed ones, including deleted ids
- [x] ThreadSanitizer clean on WAL, recovery, and engine tests (this found a test bug: freeing an uninitialized commit result after an early crash)
- [x] Full suite passes: `python3 scripts/check.py` (70 C tests in 11 executables under ASan+UBSan, web 16/16), `-Werror` clean

## M6 HTTP, WebSocket, static, auth
- [ ] Parser tests incl. malformed and oversized input
- [ ] Join, login, logout, rate limit
- [ ] WS handshake and events
- [ ] Serves the built web app

## M7 Upload, download, concurrency
- [ ] Manifest, chunks, commit, cancel, resume
- [ ] 8 concurrent CLI clients uploading overlapping files, then `--fsck` clean
- [ ] Downloads match SHA-256
- [ ] `scripts/crash_test.py` against the real server

## M8–M14
Not started (later sessions). See SPEC §14.

## Handoff notes for M6 (read before starting)

**State of the repo.** `main` is clean at the M5 commit. No GitHub remote yet; do not push.

**Engine pieces M6/M7 still owe SPEC §8 (they need the server loop):**
- *Background checkpointer:* a thread that calls `nx_engine_checkpoint` every `checkpoint_interval_s` (60 s) and when dirty frames pass 50%. Today checkpoints only run when a transaction finds the pool over 50% dirty, on `NX_BP_FULL`, and at open/close, so with light traffic the WAL grows until restart.
- *Events:* `engine.checkpoint` (duration, pages: `nx_engine_get_stats` has `last_checkpoint_ms` / `last_checkpoint_pages`) and `engine.recovery` (`nx_engine_last_recovery`).
- *`/api/health` while recovering:* recovery runs inside `nx_engine_open` today. To answer `{"status":"recovering"}` the server must start listening before opening the engine, and open it on another thread.
- *`nexusd --init`, `--fsck`, `--create-admin`:* `main.c` still only does `--version`. `nx_engine_create`, `nx_engine_fsck`, and `nx_engine_add_user` exist; config parsing (`util/config.c`) is needed first. M7's acceptance runs `--fsck`.

**Testing hooks.** `util/failpoint.h` crash points and `nx_engine_abandon` are available for M7 tests too (crash during an HTTP upload). The real-process kill test is `scripts/crash_test.py` (M7/M13).

## Open problems
- **Windows and Linux not yet verified.** `plat_win32.c` and the Linux paths of `plat_posix.c` (getrandom, MSG_NOSIGNAL) have not been compiled. They will be once the repo is pushed to GitHub and CI runs, or when a teammate runs `make -C server test` on a Windows laptop (MSYS2) or Linux.
- **macOS SDK mismatch on the dev Mac.** A leftover beta `MacOSX27.0.sdk` breaks linking. The Makefile works around it (D-009). Deleting that SDK (needs sudo) would remove the need.

## Final report
(Written at the end of the run.)
