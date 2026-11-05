# NexusSync — progress

Checklist of every build step in `docs/SPEC.md` §14. A box is ticked only after the check was actually executed and passed.
Resume after an interruption: read `CLAUDE.md`, then this file, re-run the tests of the last ticked step, continue from the first unticked item.

Platforms: macOS (verified locally on Apple Silicon), Linux and Windows (verified in CI once the repo is pushed; see "Open problems").

Current scope agreed with the team: M1–M7 in this run; M8–M14 in later sessions.

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
- [ ] Slotted-page catalog (users, files)
- [ ] Recipes append + read
- [ ] Round-trip of 200 random files including duplicates and edited copies
- [ ] I1–I3 checked by `engine_fsck`

## M5 WAL, checkpoint, recovery
- [ ] WAL encode/decode, torn-tail truncation
- [ ] Checkpoint with doublewrite; torn-checkpoint repair test
- [ ] Crash-between-steps tests (failpoints)
- [ ] In-process crash loop of 20 iterations

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

## Open problems
- **Windows and Linux not yet verified.** `plat_win32.c` and the Linux paths of `plat_posix.c` (getrandom, MSG_NOSIGNAL) have not been compiled. They will be once the repo is pushed to GitHub and CI runs, or when a teammate runs `make -C server test` on a Windows laptop (MSYS2) or Linux.
- **macOS SDK mismatch on the dev Mac.** A leftover beta `MacOSX27.0.sdk` breaks linking. The Makefile works around it (D-009). Deleting that SDK (needs sudo) would remove the need.

## Final report
(Written at the end of the run.)
