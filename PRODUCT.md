# NexusSync — product context

## One line

A storage server for a college lab network that stores every repeated piece of every file only once, and shows, live and honestly, how much disk space and network traffic that saves.

## The problem

In a college computer lab or small office, many people upload the same or nearly the same files: the same Linux ISO, the same lab manual, the same dataset, a slightly edited copy of yesterday's report. A normal file server stores every copy in full and receives every byte over the shared Wi-Fi. That wastes disk space and congests the network at exactly the moments everyone is uploading, such as just before a submission deadline.

NexusSync splits each file into small content-defined blocks, fingerprints each block, and asks the server which blocks it already has before sending anything. Only new blocks cross the network and only new blocks are written to disk. Files are rebuilt from their blocks on download, byte for byte.

## Who uses it

**Student (primary user).** Signs in from a lab PC or laptop, uploads files, downloads them later, sometimes uploads an edited version of a file. Wants uploads to be fast and files to come back exactly as they went in. Does not care about blocks or hashes, but appreciates seeing "already on the server, nothing sent".

**Lab admin.** Runs the server on one machine. Creates the join code, manages accounts, watches disk use and engine health, runs cleanup and integrity scans. Needs to trust that the server will survive a power cut.

**Examiner (the audience that decides the grade).** A professor evaluating an Operating Systems and DBMS project in about ten minutes. Technical and skeptical. Will ask where every number comes from, what happens when the server crashes, and why block-level deduplication beats simpler options. Wants to see real systems engineering, not a thin interface over an existing database.

## Academic context

- Third-year mini project at Graphic Era, covering both Operating Systems and DBMS in one system.
- Three evaluation phases: **Phase 1** is the proposal and approval, **Phase 2** shows a working core, **Phase 3** shows the finished project. The team's goal is a complete, demonstrable system by Phase 3.
- The project must demonstrate, in working code: file I/O and `fsync` semantics, processes and threads, synchronization and deadlock avoidance, sockets, page caching and replacement (OS); and indexing (extendible hashing), buffer management, transactions, write-ahead logging, checkpointing, crash recovery, and integrity checking (DBMS).
- AI-assisted development is used. Follow the department's policy on disclosing it, and make sure every teammate understands every module (see `docs/LEARNING.md`).

## What success looks like

- A second upload of an existing file finishes in seconds and sends almost nothing over the network.
- An edited file sends only the blocks around the edit.
- Killing the server with `kill -9` mid-upload loses no committed data; after restart `--fsck` passes and every file downloads with a matching SHA-256.
- Measured results on real lab data, compared against honest baselines: plain storage, gzip, whole-file deduplication, and fixed-size 4 KB blocks.
- An interface calm and precise enough that the examiner understands deduplication by looking at it.

## Scope

**In scope:** single server on Linux; browsers on any OS as clients; accounts with roles (student, admin) and a lab join code; upload, download, delete, versions, optional sharing with the lab; content-defined and fixed-size chunking; custom block index, catalog, WAL, recovery, garbage collection, compaction, integrity scan; live dashboard over WebSocket; compare-versions view; engine view for admins; CLI client for scripting, tests, and evaluation.

**Out of scope:** multiple servers or replication, access over the internet, HTTPS (documented as a limitation), encryption at rest, folders and folder sync, mobile apps, file previews, real-time collaborative editing.

## Claims we make, and claims we do not

Make only claims that are measured:
- Disk saved, network bytes saved, deduplication ratio, upload and download throughput, recovery time and correctness, all measured on the team's own datasets.

Do not claim:
- Energy, carbon, or "trees planted" savings. They cannot be measured or defended.
- That NexusSync is novel. Content-addressed block storage exists (Git objects, ZFS dedup, restic, borg, Plan 9 Venti). The contribution is a from-scratch implementation, the measured comparison, and crash safety.
- That edited PDFs, Word, PowerPoint, or zip files deduplicate well. They are compressed internally, so an edit changes most of the bytes. Show this honestly as a limitation.

## Known limitations (state these in the report and viva)

Single point of failure; no replication. Corruption of one shared block affects every file that uses it (mitigated by integrity scans and verify-on-read). Compressed or encrypted files barely deduplicate. A user can infer that someone else has uploaded a file by noticing it needs no upload (a known side channel of client-side deduplication). No encryption at rest, and HTTP without TLS on the LAN. The index grows with stored data and eventually needs disk lookups. Downloads read blocks from scattered locations, which gets slower as the store fragments.

## Voice and tone of the interface

Calm, exact, and plain. Short sentences in sentence case. Numbers with units. No exclamation marks, no hype, no emoji, no gamification. Technical terms appear only on the Compare and Engine screens, and each one is explained in a sentence nearby.

## Anti-references (what this must not look like)

Cyberpunk or "hacker terminal" dashboards with neon on black. Gamified dashboards with leaderboards, badges, or carbon counters. Generic SaaS dashboards built from grids of KPI cards. Anything with gradient text, glowing borders, glassmorphism, 3D charts, custom cursors, parallax, or numbers that count up on page load.
