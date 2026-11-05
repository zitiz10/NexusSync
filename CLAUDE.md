# NexusSync — instructions for Claude Code

Read this file at the start of every session. Then read the file for the area you are working on:

| File | What it holds |
|---|---|
| `PRODUCT.md` | What NexusSync is, who it is for, the academic context, scope, and honest claims |
| `DESIGN.md` | The visual system, every screen, every state, motion, and copy rules. The frontend must match it |
| `docs/SPEC.md` | Engine, on-disk formats, transactions, recovery, network protocol, API, auth, testing, milestones. Source of truth for behavior |
| `docs/DEMO.md` | Hardware, network, and setup for the live 2–3 laptop demo, plus the minute-by-minute demo script |
| `PROMPT.md` | The kickoff message the team gives you, plus what the humans set up before and do after your run |
| `docs/PROGRESS.md` | Your milestone checklist and final report (create it first, keep it current; resume from it after any interruption) |
| `docs/DECISIONS.md` | Running log of decisions made while building (create it in M1, append to it as you go) |
| `docs/LEARNING.md` | Plain-language explanation of each module for the team's viva (create in M1, append after every milestone) |

`PRODUCT.md` and `DESIGN.md` sit at the repo root on purpose: the Impeccable design skill reads them as project context.

## What we are building, in one paragraph

NexusSync is a LAN storage server for college labs. Files are split into content-defined blocks (about 4 KB average), each block is fingerprinted with SHA-256, and a block that already exists on the server is never sent or written again. Files are stored as ordered lists of block fingerprints ("recipes") and rebuilt on download. The server is written in C on Linux and includes its own block index (extendible hashing with a buffer pool), write-ahead log, crash recovery, garbage collection, HTTP/1.1 and WebSocket layer, and authentication. Students and the lab admin use a calm, polished web interface in any browser on the same network.

## Priorities, in order

1. **Frontend quality.** The interface must look and behave exactly as `DESIGN.md` describes, with no visual or interaction flaws. This is what examiners see first.
2. **Engine correctness.** Never lose, corrupt, or mis-assemble a file. This is a hard floor, not something to trade for polish. A beautiful UI on an engine that corrupts files fails the demo.
3. **A working end-to-end demo** on 2–3 laptops, as described in `docs/DEMO.md`.

## This is a graded student project

- Every teammate must be able to explain every module in the viva. After each milestone, append a plain-language section to `docs/LEARNING.md`: what the module does, why it is designed this way, which OS or DBMS concept it demonstrates, and two questions an examiner might ask with their answers.
- Prefer clear code over clever code. Comment the *why*, not the *what*.
- When there is a real design choice, explain the options and trade-offs in `docs/DECISIONS.md` before choosing.

## Stack

**Server (`server/`)**: C11, Linux only (Ubuntu 24.04 is the reference), gcc or clang, `make`, pthreads, OpenSSL (`libssl-dev`) for SHA-256, SHA-1 (WebSocket handshake), PBKDF2, and secure random bytes. Vendored: cJSON (MIT, single file) for parsing request JSON. No other C dependencies without asking.

**Web (`web/`)**: Node 20+, Vite, React 18, TypeScript in strict mode, React Router. Allowed packages: `vitest` and `@playwright/test` (dev only), `hash-wasm` (SHA-256 in the browser, see SPEC §6.3), `lucide-react` (icons, 1.5 px stroke), `qrcode` (connect screen). No UI kits (no MUI, Chakra, shadcn, Tailwind, Bootstrap) and no animation libraries; components and motion are hand-built from `DESIGN.md` tokens. Nothing else without recording a strong reason in `docs/DECISIONS.md`.

**CLI (`cli/`)**: C11, shares the chunker and hashing code with the server.

## Repository layout

```
nexussync/
  CLAUDE.md  PRODUCT.md  DESIGN.md
  docs/        SPEC.md  DEMO.md  DECISIONS.md  LEARNING.md
  server/
    include/   public headers
    src/
      engine/  chunk_fixed.c chunk_cdc.c gear_table.h sha.c container.c
               bufpool.c exthash.c catalog.c recipe.c wal.c checkpoint.c
               recovery.c gc.c scan.c engine.c
      net/     server.c http.c ws.c router.c static.c
      api/     auth.c files.c uploads.c compare.c stats.c admin.c events.c
      util/    log.c config.c crc32.c json_out.c time.c
      main.c
    vendor/    cJSON.c cJSON.h
    tests/     unit/ integration/ fixtures/
    Makefile
  cli/         main.c client.c  (links server/src/engine/{chunk_*,sha}.c)
  web/
    public/fonts/        Geist and Geist Mono woff2 (self-hosted)
    src/
      app/ components/ screens/ engine/ (browser chunker, worker)
      styles/tokens.css  styles/base.css
      api/ (typed client, websocket)
    index.html  vite.config.ts
  scripts/     gen_gear.py crash_test.sh demo_reset.sh make_demo_data.py fsck_all.sh
  eval/        run_eval.py  results/
  design/mockups/        screenshots of the approved mockups (added by the team)
  nexussync.conf.example
```

`data/`, `*.pem`, `nexussync.conf`, `demo.env`, and `eval/datasets/` are git-ignored.

## Commands

```bash
# server
make -C server              # release build  -> server/build/nexusd
make -C server debug        # -O0 -g
make -C server asan         # AddressSanitizer + UndefinedBehaviorSanitizer
make -C server test         # unit + integration tests (runs under asan)
./server/build/nexusd --config nexussync.conf
./server/build/nexusd --config nexussync.conf --init           # create data dir
./server/build/nexusd --config nexussync.conf --create-admin   # prompts for password
./server/build/nexusd --config nexussync.conf --fsck           # full consistency check
./server/build/nexusd --config nexussync.conf --rebuild-index  # rebuild index from containers

# web
cd web && npm ci
npm run dev       # Vite dev server on :5173, proxies /api and /ws to :8080
npm run build     # outputs web/dist, served by nexusd at runtime
npm test          # vitest (chunker parity, reducers)
npm run e2e       # Playwright end-to-end

# tooling
python3 scripts/gen_gear.py           # regenerates gear tables for C and TS from one seed
./scripts/crash_test.sh 50            # 50 random kill -9 cycles, fsck after each
./scripts/demo_reset.sh               # wipe and seed a clean demo server
python3 eval/run_eval.py eval/datasets # baselines and charts into eval/results
```

## How to work: one autonomous run

The team will start you once with `PROMPT.md` and expects you to build the whole project end to end without stopping for approval. Work like this:

- **Follow the build order** in `docs/SPEC.md` §14, M1 to M14, in sequence. Do not skip ahead to the UI before the engine milestones pass.
- **Track progress in `docs/PROGRESS.md`.** Create it first as a checklist of every milestone and its acceptance checks. Tick items only when verified. If the session ends or context is compacted, a new session resumes by reading `CLAUDE.md` and `docs/PROGRESS.md` and continuing from the first unticked item.
- **Verify, do not assume.** A milestone is complete only when its acceptance checks pass, `make -C server asan test` is clean, and you have actually run the thing (server started, endpoint called, screen screenshotted). Never tick a box on the basis of code you have not executed.
- **Fix before moving on.** If a test fails, find the root cause and fix it. Do not weaken, skip, or delete a test to make it pass. If something truly cannot pass, record it under "Open problems" in `docs/PROGRESS.md` with what you tried, and continue with the next milestone.
- **Decide, do not ask.** Where the spec is silent, choose the simplest option consistent with it, record the choice and the alternatives in `docs/DECISIONS.md`, and continue. Only stop for a real blocker you cannot work around (for example, a missing system package that needs `sudo`), and say exactly what the human must do.
- **Stay inside the spec.** Do not add features, dependencies beyond the allowed list, or stretch goals (SPEC §18) until M14 is done.
- **Commit after every milestone** with a conventional message (`feat(engine): ...`, `fix(web): ...`), so the team can review history milestone by milestone.
- **No fake data in the product.** Every number on screen comes from the server. Frontend fixtures are allowed only in development behind `VITE_MOCK=1`, and the production build must fail if mock code is included.
- **Formats are contracts.** If you must change an on-disk format, the WAL layout, the chunker parameters, or the API, update `docs/SPEC.md` in the same commit and bump the version.
- **Write `docs/LEARNING.md` as you go**, one section per milestone, so the team can study the code afterwards.

## Git Commit Guidelines
- Never append "Co-Authored-By" trailers to git commit messages.
- Do not add any AI generator footers or attribution text to commits or PRs.

## Frontend rules

- Build from `web/src/styles/tokens.css`. No raw hex values, font sizes, radii, or durations in component code; use tokens.
- **Look at what you build.** After finishing each screen, run it against a real `nexusd` with seeded data, capture Playwright screenshots at 1440×900, 1366×768, and 390×844 into `web/screenshots/`, open the images, and compare them with `DESIGN.md` section by section. Fix every mismatch, then screenshot again once. Also capture the empty, loading, error, and offline states.
- **Use the Impeccable skill** if it is installed in this environment: run its critique and audit on each finished screen and fix every P0 and P1. Run its polish pass across the app in M14. If it is not installed, apply `DESIGN.md` §11–§13 as the checklist. (The team can install it with `/plugin marketplace add pbakaus/impeccable`, then install it from `/plugin`.)
- Check every screen at 1440×900, 1366×768 (common projector), and 390×844 (phone), in light mode first.
- Hover interactions are mouse-first by the team's choice, but buttons and links must keep visible focus styles.

## C rules

- Check every return value, including `malloc`, `write`, `fsync`, and `close`. Handle short reads and short writes.
- Treat all network input as hostile: bounds-check every length, cap every buffer, never trust a client-supplied hash, size, index, or path.
- On-disk structures are serialized field by field in little-endian with fixed-width integers. Never `fwrite` a C struct.
- No variable-length arrays. Use `size_t` for sizes, `uint64_t` for offsets and LSNs.
- Follow the lock order in `docs/SPEC.md` §9 exactly. Document any new lock there first.
- Logging: one structured line per event to stderr (`ts level module msg key=value`). Never log passwords, session tokens, join codes, or file contents.

## Definition of finished

Your run is finished when every milestone in `docs/SPEC.md` §14 passes its acceptance checks, `./scripts/crash_test.sh 50` passes, every screen has been screenshotted and checked against `DESIGN.md`, `docs/PROGRESS.md` is fully ticked (or lists open problems honestly), and `docs/LEARNING.md` covers every module. End with a short report in `docs/PROGRESS.md`: what works, what was verified and how, known issues, and the exact commands to start the demo.

After your run, the team will review the code, test on real laptops following `docs/DEMO.md`, and come back with fixes. Make that easy: clear commits, honest progress notes, no hidden shortcuts.
