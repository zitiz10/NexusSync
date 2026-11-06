# NexusSync — live demo guide

How to show NexusSync working on 2–3 laptops in front of examiners, what you need, and what to do if something fails.

## 1. Requirements

**Server laptop (1)**
- Any of: Windows 10/11, macOS 13 or newer, or Linux (Ubuntu 24.04). `nexusd` runs natively on all three; no virtual machine or WSL is needed.
- 8 GB RAM or more, SSD with at least 30 GB free.
- Build tools:
  - Windows: MSYS2 with `pacman -S mingw-w64-ucrt-x86_64-gcc make python` (run from the "MSYS2 UCRT64" shell).
  - macOS: Xcode Command Line Tools (`xcode-select --install`) and Python 3.
  - Linux: `build-essential python3`.
  - All: Node.js 22.12+ (only to build the web app); `matplotlib` for the evaluation charts (`python3 -m pip install --user matplotlib`).
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
- `submission.tar.gz` and `submission_v2.tar.gz` (the same tree as one gzip stream, with the first file changed). Shows the honest limitation: after an edit, a compressed stream changes all the way to its end, so almost nothing is reused.
- `submission.zip` and `submission_v2.zip` (the same tree zipped, with one change). A good examiner question: zip compresses each member on its own, so the unchanged members still deduplicate (measured in M13 on the full demo data: the v2 zip sent 67 KB of 6.3 MB, the v2 tar.gz 4.7 MB of 5.3 MB). Formats built on zip (docx, xlsx, pptx) behave the same way only when their parts are unchanged.
- Optional: a Linux ISO of 300–700 MB downloaded in advance for a large identical-upload moment.

Use CSV, text, or tar for "edited file" examples. Do not use PDF, Word, or PowerPoint for that: they are compressed internally, so small edits change most bytes.

Copy `demo-data/` to both client laptops in advance.

## 3. Setup

### 3.1 Build and start (same on every OS)
In a terminal (Windows: the "MSYS2 UCRT64" shell):
```bash
git clone <repo> && cd nexussync
make -C server && (cd web && npm ci && npm run build)
cp nexussync.conf.example nexussync.conf
python3 scripts/demo_reset.py      # wipes data, creates admin, riya, arjun from demo.env, starts nexusd
```
The Engine screen lists the server's LAN addresses with QR codes. To find the address by hand: `ipconfig` (Windows), `ipconfig getifaddr en0` (macOS), `ip -4 addr` (Linux).

### 3.2 Let clients reach the server (firewall)
- **Windows:** the first time `nexusd.exe` starts, Windows Defender Firewall asks whether to allow it. Tick **Private networks** and allow. If you missed the prompt, allow inbound TCP 8080 in "Windows Defender Firewall with Advanced Security". Set your router's network to *Private* in Settings → Network, otherwise Windows blocks inbound connections.
- **macOS:** if the firewall is on, macOS asks to allow incoming connections for `nexusd` the first time. Click Allow. Otherwise check System Settings → Network → Firewall.
- **Linux:** `sudo ufw allow 8080/tcp` if ufw is enabled.
- From a client, open `http://<server-ip>:8080/api/health` to confirm.

### 3.3 Optional: WSL2 on Windows
Only needed if the native Windows build fails on your machine. In `%UserProfile%\.wslconfig` set `[wsl2]` and `networkingMode=mirrored`, run `wsl --shutdown`, reopen Ubuntu, then follow §3.1 inside it.

### 3.4 Clients
Open `http://<server-ip>:8080` (scan the QR code from the Engine screen), sign in as `riya` or `arjun`. Keep the `demo-data` folder open in the file manager.

## 4. Demo script (about 10 minutes)

| Time | Who | Action | What examiners see | What to say |
|---|---|---|---|---|
| 0:00 | Presenter | Projector on Overview, empty server | "Nothing stored yet." and 384 empty tiles | The problem: labs store and send the same data again and again |
| 0:45 | A | Upload `dataset.csv` | Tray phases: fingerprinting, checking, sending all blocks; tiles fill on the projector live | Every block is fingerprinted in the browser; the server is asked first |
| 2:00 | B | Upload the same `dataset.csv` | Finishes in seconds, "Stored. 0 bytes sent instead of 300 MB" | Only fingerprints crossed the network |
| 2:45 | B | Upload `dataset_v2.csv` | A few blocks sent; becomes version 2 | Content-defined blocks: only blocks near the edit change |
| 3:30 | Presenter | Compare versions, starts in fixed 4 KB mode, then toggle | Long peach trail in fixed mode, then almost all blue in content-defined mode; hover a block to show its partner | Why fixed-size blocks fail after an insertion, and how a rolling hash fixes it |
| 5:00 | A | Upload `submission_v2.tar.gz` after `submission.tar.gz` | Mostly peach, little reuse | Honest limitation: one compressed stream hides repetition after an edit (a zip, which compresses each file on its own, does better) |
| 5:45 | A + Engineer | Start uploading the ISO or `project_src.tar`; Engineer hard-kills the server mid-upload (`kill -9 $(pgrep nexusd)` on macOS/Linux, `taskkill /F /IM nexusd.exe` on Windows), then restarts | Clients show "Lost connection, retrying"; projector shows recovery with records replayed; then normal | Write-ahead log, commit point, redo recovery, no-steal buffer pool |
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
- **State gets messy:** `python3 scripts/demo_reset.py` returns to a clean server in under a minute. Optionally keep a snapshot tarball of `data/` taken after the first upload to restore mid-demo.
- Record a full run-through on video the day before. Bring it on a USB drive.

## 6. Checklists

**A week before:** network tested with all laptops and the router; firewall prompt accepted on the server laptop; demo data generated and copied; full rehearsal timed; video recorded.

**The day before:** fresh build from the main branch; `make -C server test` and `python3 scripts/crash_test.py 20` pass; `demo_reset.py` run; laptops charged; sleep disabled; browser zoom checked on the projector resolution.

**One hour before:** router on; server started with `demo_reset.py`; all clients signed in; Overview on the projector; terminal open with the `kill -9` and restart commands ready; `demo-data` folders open on both clients; notifications muted on all laptops.

## 7. What to show at each phase

- **Phase 1 (approval):** the problem, measured duplication in real lab data from the validation script, the architecture diagram, the mockups, the plan and milestones. No live demo needed.
- **Phase 2:** server running on one laptop, one or two clients. Identical upload sends nothing, edited CSV sends little, `kill -9` and recovery with `--fsck`, download hash check. The interface may be partly finished, but everything shown must be real.
- **Phase 3:** the full script in §4 with the finished interface and evaluation results.
