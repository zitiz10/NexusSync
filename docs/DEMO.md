# NexusSync — live demo guide

How to show NexusSync working on 2–3 laptops in front of examiners, what you need, and what to do if something fails.

## 1. Requirements

**Server laptop (1)**
- Linux. Ubuntu 24.04 installed natively is strongly recommended (dual boot is fine).
- If only Windows is available: Windows 11 with WSL2 and Ubuntu 24.04, with mirrored networking enabled (see §3.2). Test this at least a week before any phase review, because WSL networking is the most likely thing to fail on the day.
- 8 GB RAM or more, SSD with at least 30 GB free.
- Build tools: `build-essential`, `libssl-dev`, `python3`, `python3-matplotlib`, Node.js 20+ (only to build the web app).
- A charger. Disable sleep and screen lock during the demo.

**Client laptops (1–2)**
- Any OS with a current Chrome, Edge, or Firefox. Nothing to install.
- For the download check: `sha256sum` (Linux), `shasum -a 256` (macOS), or `certutil -hashfile <file> SHA256` (Windows).

**Network**
- Bring your own: any Wi-Fi router (no internet needed) or a phone hotspot. College and office Wi-Fi often enables client isolation, which blocks laptops from reaching each other; do not rely on it.
- Best: server laptop connected to the router by Ethernet, clients on Wi-Fi.

**Projector**
- The server laptop (or a third laptop signed in as admin) drives the projector showing the Overview or Engine screen. Check at 1366×768 and 1024×768.

**People (4-person team)**
- Presenter: talks, drives the projector laptop.
- Operator A: laptop A, signed in as `riya`.
- Operator B: laptop B, signed in as `arjun`.
- Engineer: server terminal for the crash demo and `--fsck`, answers engine questions.

## 2. Demo data

Generate with `python3 scripts/make_demo_data.py demo-data/` (deterministic, so results repeat):
- `dataset.csv` (about 300 MB) and `dataset_v2.csv` (same, with 50 rows inserted near the start). Shows an edited file sending very little.
- `project_src.tar` (about 50 MB) and `project_src_v2.tar` (one source file changed).
- `lab_manual.txt` (about 5 MB) uploaded by both students. Shows an identical copy sending nothing.
- `submission.zip` and `submission_v2.zip` (zip of the same tree with one change). Shows the honest limitation: compressed files barely deduplicate.
- Optional: a Linux ISO of 300–700 MB downloaded in advance for a large identical-upload moment.

Use CSV, text, or tar for "edited file" examples. Do not use PDF, Word, or PowerPoint for that: they are compressed internally, so small edits change most bytes.

Copy `demo-data/` to both client laptops in advance.

## 3. Setup

### 3.1 Server on native Ubuntu
```bash
git clone <repo> && cd nexussync
make -C server && (cd web && npm ci && npm run build)
cp nexussync.conf.example nexussync.conf
sudo ufw allow 8080/tcp            # if ufw is enabled
./scripts/demo_reset.sh            # wipes data, creates admin, riya, arjun from demo.env, starts nexusd
ip -4 addr                          # note the LAN address, e.g. 192.168.1.20
```
The Engine screen also lists the server's LAN addresses with QR codes.

### 3.2 Server on Windows 11 with WSL2
1. In `%UserProfile%\.wslconfig`:
   ```
   [wsl2]
   networkingMode=mirrored
   ```
2. Run `wsl --shutdown`, reopen Ubuntu, then follow §3.1.
3. Allow inbound TCP 8080 in Windows Defender Firewall if clients cannot connect.
4. From a client, open `http://<windows-laptop-ip>:8080/api/health` to confirm.
If this does not work reliably after an evening of trying, switch the server laptop to native Ubuntu.

### 3.3 Clients
Open `http://<server-ip>:8080` (scan the QR code from the Engine screen), sign in as `riya` or `arjun`. Keep the `demo-data` folder open in the file manager.

## 4. Demo script (about 10 minutes)

| Time | Who | Action | What examiners see | What to say |
|---|---|---|---|---|
| 0:00 | Presenter | Projector on Overview, empty server | "Nothing stored yet." and 384 empty tiles | The problem: labs store and send the same data again and again |
| 0:45 | A | Upload `dataset.csv` | Tray phases: fingerprinting, checking, sending all blocks; tiles fill on the projector live | Every block is fingerprinted in the browser; the server is asked first |
| 2:00 | B | Upload the same `dataset.csv` | Finishes in seconds, "Stored. 0 bytes sent instead of 300 MB" | Only fingerprints crossed the network |
| 2:45 | B | Upload `dataset_v2.csv` | A few blocks sent; becomes version 2 | Content-defined blocks: only blocks near the edit change |
| 3:30 | Presenter | Compare versions, starts in fixed 4 KB mode, then toggle | Long peach trail in fixed mode, then almost all blue in content-defined mode; hover a block to show its partner | Why fixed-size blocks fail after an insertion, and how a rolling hash fixes it |
| 5:00 | A | Upload `submission_v2.zip` after `submission.zip` | Mostly peach, little reuse | Honest limitation: compressed files hide repetition |
| 5:45 | A + Engineer | Start uploading the ISO or `project_src.tar`; Engineer runs `kill -9 $(pidof nexusd)` mid-upload, then restarts | Clients show "Lost connection, retrying"; projector shows recovery with records replayed; then normal | Write-ahead log, commit point, redo recovery, no-steal buffer pool |
| 7:00 | Engineer | `./server/build/nexusd --config nexussync.conf --fsck` | "All checks passed" with counts | Invariants I1–I3 verified on disk |
| 7:30 | A | Retry the interrupted upload | Completes quickly; blocks sent before the crash were kept | Resuming is free because blocks are content-addressed |
| 8:00 | B | Download `dataset_v2.csv`, run the SHA-256 command, compare with the file drawer | Hashes match | The file is rebuilt byte for byte from shared blocks |
| 8:45 | Presenter | Engine screen | WAL, index depth and buffer hit rate, containers, integrity scan | Where each OS and DBMS concept lives |
| 9:30 | Presenter | Evaluation slide | Charts against baselines | Measured results, not claims |

Keep one browser tab per client laptop. Do not refresh during the crash step; the app reconnects on its own.

## 5. If something goes wrong

- **Clients cannot reach the server:** check both are on your router, ping the server IP, check the firewall, try `http://<ip>:8080/api/health`. Fallback: run the whole demo on the server laptop with two browser profiles (one per student) and the projector.
- **Server crashes unexpectedly:** restart it. Recovery is part of the demo; say so calmly.
- **A step fails twice:** move on and show the recorded video of that step.
- **State gets messy:** `./scripts/demo_reset.sh` returns to a clean server in under a minute. Optionally keep a snapshot tarball of `data/` taken after the first upload to restore mid-demo.
- Record a full run-through on video the day before. Bring it on a USB drive.

## 6. Checklists

**A week before:** network tested with all laptops and the router; WSL networking verified if used; demo data generated and copied; full rehearsal timed; video recorded.

**The day before:** fresh build from the main branch; `make -C server test` and `./scripts/crash_test.sh 20` pass; `demo_reset.sh` run; laptops charged; sleep disabled; browser zoom checked on the projector resolution.

**One hour before:** router on; server started with `demo_reset.sh`; all clients signed in; Overview on the projector; terminal open with the `kill -9` and restart commands ready; `demo-data` folders open on both clients; notifications muted on all laptops.

## 7. What to show at each phase

- **Phase 1 (approval):** the problem, measured duplication in real lab data from the validation script, the architecture diagram, the mockups, the plan and milestones. No live demo needed.
- **Phase 2:** server running on one laptop, one or two clients. Identical upload sends nothing, edited CSV sends little, `kill -9` and recovery with `--fsck`, download hash check. The interface may be partly finished, but everything shown must be real.
- **Phase 3:** the full script in §4 with the finished interface and evaluation results.
