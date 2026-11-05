# NexusSync — progress

Checklist of every build step in `docs/SPEC.md` §14. A box is ticked only after the check was actually executed and passed.
Resume after an interruption: read `CLAUDE.md`, then this file, re-run the tests of the last ticked step, continue from the first unticked item.

Platforms: macOS (verified locally on Apple Silicon), Linux and Windows (verified in CI once the repo is pushed; see "Open problems").

Sessions so far: session 1 built M1–M3; session 2 built M4–M8. **The next session starts at M9** and continues to M14.

**Status (2026-11-06): M1–M8 done and committed. Next: M9 (Overview, Files, upload tray).** Read "Handoff notes for M9–M14" below before starting.

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
- [x] Parser tests incl. malformed and oversized input: 20 malformed heads (400/411/417/501/505), NUL, 16 KB head (431), 9 KB target (414), 70 headers (431), every split point, 2,000 random heads under ASan; over the wire: garbage, chunked (411), 19 KB header (431), 2 MB body refused unread (413), bad JSON (400), wrong content type (415), server healthy afterwards; keep-alive and pipelining
- [x] Join, login, logout, rate limit: bad/rotated join code, weak password, bad username, taken name, CSRF header required, case-insensitive code, lower-cased usernames, admin-only routes, disabled account loses its session; 5 fails per user and 20 per IP give 429 (also blocks join from that IP)
- [x] WS handshake and events: RFC 6455 accept-key vector, no session (401), foreign Origin (403), version 8 (426), engine.checkpoint delivered, ping/pong, unmasked frame closed with 1002, clean close, server.shutdown on stop; checked again in Chrome (HttpOnly cookie invisible to JS, Origin accepted, event received)
- [x] Serves the built web app: index (no-cache), hashed assets (immutable), client-side routes, traversal refused (6 variants); real browser loads `web/dist` with no CSP errors
- [x] `nexusd --init`, `--create-admin`, `--fsck`, serve with SIGTERM shutdown, all run for real; `/api/health` says "recovering" during recovery and other API calls get 503
- [x] Busy server answers 503 (max_connections); ThreadSanitizer clean on 5 runs of the server tests
- [x] Full suite: `python3 scripts/check.py` passes (87 C tests in 13 executables under ASan+UBSan, web 16/16), `-Werror` clean

## M7 Upload, download, concurrency
- [x] Manifest, chunks, commit, cancel, resume: full protocol over sockets with every validation (names, sizes, chunk counts, hashes, pages, per-chunk `unknown_index` / `length_mismatch` / `hash_mismatch`, 409 `manifest_incomplete` and `missing_chunks` with indices, 422 `file_hash_mismatch`, 429 session limit); identical re-upload sends 0 bytes; edited copy sent 4.8 KB of 300 KB; versions and `versionOf: null`; resume needed 27 of 53 blocks; cancel (3/3 tests)
- [x] Files API: list (chain heads, q, sort, scope, paging), detail with versions and block counts, streamed download with `Content-Disposition`, HEAD, share/unshare, delete; access rules (owner, shared read-only, admin; others get 404); bad ids rejected by the router
- [x] `nexus` CLI (login, put, get, ls, rm; session file mode 0600), checked against a real server: dedup, CDC vs fixed after an edit (1 vs 417 new blocks), byte-identical download
- [x] 8 concurrent CLI clients uploading overlapping files, then `--fsck` clean: `scripts/concurrency_test.py`, 56 files / 72 MB in under 1 s, no warnings in the server log, part of `check.py`
- [x] Downloads match SHA-256: every file in the concurrency and crash tests downloaded and compared; `nexus get` verifies and discards mismatches
- [x] `scripts/crash_test.py` against the real server: 50 rounds (seed 7) clean: 4,685 operations, 121 uploads in flight at a kill, each all-or-nothing, fsck clean 50/50; rerun on the final code (seed 11): 50/50 clean, 4,226 operations, 108 uploads in flight at a kill
- [x] Sessions survive a kill (D-030), so the demo's crash step needs no re-login; persistence unit-tested (hashes only, damaged file handled)
- [x] ThreadSanitizer clean on server, upload, net, engine, and index tests after fixing a real race it found in index statistics (D-033)
- [x] Full suite: `python3 scripts/check.py` passes (91 C tests in 14 executables under ASan+UBSan, concurrency test, web 16/16), `-Werror` clean

## M8 Frontend foundation
- [x] Tokens (DESIGN §2, plus the M8 additions recorded in DESIGN), self-hosted Geist and Geist Mono (OFL), base styles with themed selection, caret, focus, and scrollbars; no raw values in component code
- [x] Shell: brand, nav (admin-only Engine/Users), status indicator ("Live, updated N s ago", Reconnecting, Offline), user menu (theme Light/Dark/System persisted, sign out), skip link; phone layout on two rows
- [x] Sign in and join: labels above fields, rules as hints, inline errors from client checks and from server codes, show/hide password, rate-limit and session-ended messages
- [x] API client (CSRF header, typed errors, 401 = session ended) and WebSocket client with backoff 1, 2, 4, 8, 10 s; refetch epoch after a reconnect or a recovery
- [x] System states: offline banner with dimmed data, recovering and recovery-complete pages, unreachable page, loading skeleton, session ended, sign-out refused while offline
- [x] Screenshots at 1440×900, 1366×768, 390×844 (and dark) via Playwright against a real nexusd (`cd web && npm run e2e`, 18 tests): checked against DESIGN, fixed, re-shot once
- [x] Impeccable critique (independent sub-agent, 29/40) and audit (computed contrast tables, keyboard, ARIA): all P0/P1 fixed; two token contrast failures fixed in DESIGN (D-036); remaining items listed in D-039
- [x] Tests: web unit 29/29, Playwright 18/18 (including a real SIGKILL and reconnect while staying signed in), `python3 scripts/check.py` passes, typecheck strict, `npm audit` 0

## M9 Overview, Files, upload tray
- [ ] Server: `GET /api/stats` (SPEC §12) and the `stats.updated` event (2/s max)
- [ ] Server: `GET /api/blockmap` (+ `?file=`), and real `newTiles` in `upload.progress`
- [ ] Browser chunker + hashing in a Web Worker (`web/src/engine/chunker.ts` exists, parity-tested in M2), 8 MB windows
- [ ] Overview: headline, block field (384 tiles, entrance motion, linked hover, live pulses), Recent uploads, right column (admin engine summary / student storage), empty state
- [ ] Files: list with search, sort, scope, paging; drawer (versions, blocks, SHA-256 copy, download, delete with 6 s undo toast); share toggle; empty and no-results states
- [ ] Upload tray: phases Reading → Checking → Sending → Verifying → Done; failures (connection lost + resume, hash mismatch, disk full, session ended); cancel; drag and drop anywhere
- [ ] Screenshots of every state at three sizes, critique and audit, Playwright flows

## M10 Compare versions
- [ ] Server: `GET /api/compare` with fixed and CDC modes, window, minimap, block details, 5 min cache
- [ ] Screen per DESIGN §8.5 (defaults to fixed, pickers, window, minimap, hover partners, details, motion)

## M11 Engine and Users screens, system states
- [ ] Server: remaining `/api/admin/*` (users, events, recoveries, addresses, gc, scan; join code is done)
- [ ] Engine screen (DESIGN §8.6) with live data and actions; Users screen (§8.7); QR codes (`qrcode` package)
- [ ] Resolve D-039 items 1–2 (offline dimming contrast; live recovery counts)

## M12 Cleanup, compaction, integrity
- [ ] GC respecting the grace period and open upload sessions (`nx_uploads_declares` exists), compaction surviving a crash mid-way, scan detecting an injected corrupt byte, nightly schedule, `--rebuild-index`

## M13 CLI, demo tooling, evaluation
- [ ] `nexus stats`; `scripts/make_demo_data.py`, `scripts/demo_reset.py`; `eval/run_eval.py` with tables and charts; `crash_test.py 50` (already passing since M7)

## M14 Polish and final report
- [ ] Polish pass across the app (Impeccable `polish`, with the plugin's detector installed); every screen and state at three sizes per DESIGN §13; Playwright end-to-end flow of SPEC §15; final report below

## Handoff notes for M9–M14 (read before starting)

**How to resume.** Read `CLAUDE.md`, then this file, then `DESIGN.md` §8.2–§8.4 and SPEC §12 for M9. Re-run the last step's tests first:
```bash
python3 scripts/check.py          # C tests (ASan), 8-client concurrency test, web unit tests
cd web && npm run e2e             # builds the app, starts a real nexusd, 18 Playwright tests
```
Both passed at the M8 commit. No GitHub remote yet; do not push. Commit after each milestone (no Co-Authored-By trailers, CLAUDE.md).

**Packages and tools already approved and installed:** cJSON (vendored), Geist fonts, `lucide-react`, `@playwright/test` (uses the installed Google Chrome, `channel: 'chrome'`). Still allowed but not installed: `qrcode` (M11). Downloads need the user's OK each time.

**Frontend map (web/src).**
- `styles/tokens.css`: the only place for values.
- `styles/components.css`: buttons, fields, nav, status, menu, banner, skeleton, auth, calm.
- `api/client.ts`: typed `request()` and `api.*` endpoints. Add new endpoints here.
- `api/ws.ts`: `LiveSocket`.
- `app/connection.tsx`:
  - `useConnection()` gives `server`, `link`, `lastUpdated`, and `epoch` (refetch when it changes);
  - `useServerEvent(type, fn)` subscribes to one event type;
  - `useNow()` re-renders on a timer.
- `app/auth.tsx`: `useAuth()` and `useUser()`.
- `app/theme.tsx`, `app/format.ts` (`formatBytes`, `formatAgo`), `app/validate.ts`.
- `components/`: Button (with loading), TextField, Mark/Brand, StatusIndicator, UserMenu, OfflineBanner, icons.
- `screens/`: Shell, SignIn, Join, SystemStates. `Pages.tsx` holds the "Not available yet" pages for M9–M11; replace each one when you build its screen.

**Server map for the new endpoints.**
- Routes: the `ROUTES` table in `server/src/net/router.c` (`{id}` and `{uid}` parameters; the state, CSRF, auth, and admin gates are automatic).
- Handlers in `server/src/api/`; JSON with `util/json_out.h`; events with `nx_hub_publish`.
- Engine stats: `nx_engine_get_stats`. Index walk: `nx_exthash_for_each` (hold the shared directory lock). Recipes: `nx_engine_file_recipe`.
- `upload.progress` already carries `newTiles: []`: fill it once the block map exists (M9).

**Testing patterns to reuse.**
- C: `tests/nx_server_util.h` (in-process server and raw client); `tests/integration/test_uploads.c` shows the upload protocol.
- Web: `web/e2e/server.ts` (start, kill, restart the real server); screenshots go to `web/screenshots/<milestone>/`.

**Open design questions for the team (D-039):** offline dimming contrast; live recovery counts. Both are scheduled in M11 above.

## Open problems
- **Windows and Linux not yet verified.** `plat_win32.c` and the Linux paths of `plat_posix.c` (getrandom, MSG_NOSIGNAL) have not been compiled. They will be once the repo is pushed to GitHub and CI runs, or when a teammate runs `make -C server test` on a Windows laptop (MSYS2) or Linux.
- **macOS SDK mismatch on the dev Mac.** A leftover beta `MacOSX27.0.sdk` breaks linking. The Makefile works around it (D-009). Deleting that SDK (needs sudo) would remove the need.

## Final report
(Written at the end of the run.)
