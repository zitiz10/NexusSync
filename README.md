# NexusSync

A storage server for college lab networks that stores every repeated piece of every file only once, and shows, live, how much disk space and network traffic that saves.

Files are split into content-defined blocks (about 4 KB on average) and each block is fingerprinted with SHA-256. Before uploading, the browser asks the server which blocks it already has, so only new blocks cross the network and only new blocks reach the disk. Files are stored as ordered lists of fingerprints ("recipes") and rebuilt byte for byte on download.

The server is written from scratch in portable C11 and runs on Linux, macOS and Windows. It has its own block index, write-ahead log, crash recovery and HTTP/WebSocket layer, with no database and no OpenSSL. Students and the lab admin use it from any browser on the same network.

> Third-year Operating Systems + DBMS mini project, Graphic Era. Built with AI assistance; every module is explained in plain language in [`docs/LEARNING.md`](docs/LEARNING.md).

## What it does

- **Block-level deduplication.** A second upload of the same file sends almost nothing. An edited file sends only the blocks around the edit.
- **Survives crashes.** Committed data survives `kill -9` or a power cut: on restart the server replays its write-ahead log, and `--fsck` checks every file.
- **Live dashboard.** Upload progress, bytes saved, and a map of stored blocks update over WebSocket.
- **Compare versions.** See which blocks two versions of a file share and how much the new version actually added.
- **Admin tools.** User management with a lab join code, cleanup of unused blocks with container compaction, an integrity scan, and engine health stats. Cleanup runs nightly.
- **CLI client.** `nexus put/get/ls/rm/stats` for scripting, tests and evaluation.

## OS and DBMS concepts in working code

| Area | Where |
|---|---|
| Content-defined chunking (FastCDC, gear hash) | `server/src/engine/chunk_cdc.c` |
| Extendible-hashing block index | `server/src/engine/exthash.c` |
| Buffer pool with Clock replacement, no-steal | `server/src/engine/bufpool.c` |
| Slotted-page catalog, file recipes | `catalog.c`, `recipe.c` |
| Redo-only write-ahead log, transactions | `wal.c`, `logrec.c`, `engine.c` |
| Checkpoints with a doublewrite buffer, crash recovery | `checkpoint.c`, `engine.c` |
| Garbage collection and compaction | `gc.c` |
| Integrity scan, `--fsck`, `--rebuild-index` | `scan.c`, `engine.c` |
| Threads, locks and a documented lock order | `docs/SPEC.md` §9 |
| `fsync` semantics across platforms | `server/src/platform/` |
| Sockets, HTTP/1.1, WebSocket | `server/src/net/` |
| SHA-256, HMAC, PBKDF2 (in-repo, tested against NIST/RFC vectors) | `server/src/crypto/` |

## Repository layout

```
server/   C11 server: engine, network layer, API, platform layer, crypto, tests
cli/      nexus command-line client (shares the server's chunker and hashing)
web/      React + TypeScript web app, Playwright end-to-end tests
scripts/  checks, concurrency and crash tests, demo data and reset
eval/     evaluation against baselines (plain, gzip, whole-file, fixed 4 KB blocks)
docs/     specification, demo script, decisions, learning notes, progress
```

## Getting started

Requirements: a C11 compiler with `make` (on Windows, MSYS2 UCRT64), Node 22.12+, and Python 3.

```bash
make -C server                       # builds server/build/nexusd and the nexus CLI
(cd web && npm ci && npm run build)  # builds the web app that nexusd serves
cp nexussync.conf.example nexussync.conf
./server/build/nexusd --config nexussync.conf --init
./server/build/nexusd --config nexussync.conf --create-admin
./server/build/nexusd --config nexussync.conf
```

Then open `http://<server-ip>:<port>` (the port is set in `nexussync.conf`) in a browser on the same network.

For a ready-made demo with seeded users and files:

```bash
python3 scripts/make_demo_data.py demo-data/
python3 scripts/demo_reset.py        # passwords are written to demo.env
```

## Testing

```bash
make -C server test                  # C unit and integration tests (ASan + UBSan where available)
make -C server tsan                  # ThreadSanitizer
python3 scripts/check.py             # everything: C tests, 8-client concurrency test, web tests
python3 scripts/crash_test.py 50     # 50 random hard kills, fsck after each
cd web && npm test && npm run e2e    # unit tests and Playwright against a real server
```

CI builds and tests on Ubuntu, macOS and Windows (`.github/workflows/ci.yml`).

## Known limits

- HTTP without TLS, for use on a trusted lab LAN only.
- No encryption at rest, no replication, no folders.
- Evaluation numbers so far come from synthetic demo data; see [`eval/results/README.md`](eval/results/README.md).

More in [`docs/PROGRESS.md`](docs/PROGRESS.md).

## Documentation

- [`PRODUCT.md`](PRODUCT.md): who it is for and what success looks like
- [`DESIGN.md`](DESIGN.md): the visual system and every screen
- [`docs/SPEC.md`](docs/SPEC.md): engine, on-disk formats, recovery, protocol, API
- [`docs/DEMO.md`](docs/DEMO.md): the 2–3 laptop live demo
- [`docs/DECISIONS.md`](docs/DECISIONS.md): design decisions and trade-offs
- [`docs/LEARNING.md`](docs/LEARNING.md): every module explained for the viva
