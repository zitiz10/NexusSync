# NexusSync — how to start the build

This project is meant to be built by Claude Code in one long autonomous run, then reviewed and tested by the team. This file has three parts: what to set up first, the message to paste, and what to do after.

## 1. Before you start (humans, about 30 minutes)

Do these first so the run does not stop on something that needs your password or a decision.

1. **A development machine.** macOS, Windows (MSYS2 UCRT64 shell), or Linux. Claude Code runs here.
2. **System packages** (install them yourself; some need an admin password):
   - macOS: `xcode-select --install`; Python 3; `python3 -m pip install --user matplotlib`
   - Windows: MSYS2, then `pacman -S mingw-w64-ucrt-x86_64-gcc make python git`
   - Linux: `sudo apt install -y build-essential clang python3 python3-matplotlib zip git curl`
3. **Node.js 22.12 or newer**, then the Playwright browser and its system libraries:
   ```bash
   node --version          # must be 22.12+
   npx -y playwright install --with-deps chromium
   ```
4. **The repo.** Create an empty folder, unzip these files into it, run `git init`, make a first commit.
5. **Mockup screenshots.** Put screenshots of the approved Overview and Compare mockups in `design/mockups/` (Claude Code cannot open claude.ai links).
6. **Impeccable (optional but recommended).** In Claude Code: `/plugin marketplace add pbakaus/impeccable`, then install it from `/plugin`.
7. **Permissions.** Configure Claude Code so it can edit files and run build, test, and server commands in this folder without asking each time; otherwise the run will pause on every command. See the Claude Code documentation on permissions and settings: https://docs.claude.com/en/docs/claude-code/overview
8. **Internet access** for the run (npm packages and the Geist font files).

## 2. The kickoff message

Open Claude Code in the repo folder and paste:

```
You are building NexusSync end to end in this repository, in one autonomous run.

1. Read CLAUDE.md, PRODUCT.md, DESIGN.md, docs/SPEC.md, and docs/DEMO.md completely before writing any code.
2. Create docs/PROGRESS.md as a checklist of every build step M1–M14 from docs/SPEC.md §14 and its acceptance checks.
3. Work through M1 to M14 in order without stopping for my approval. Follow the rules in CLAUDE.md: verify by running things, fix root causes, never weaken tests, record decisions in docs/DECISIONS.md, commit after each step, write docs/LEARNING.md as you go.
4. Engine correctness first: do not start the frontend (M8) until M1–M7 pass, including the crash tests.
5. For the frontend, screenshot every screen and state at 1440×900, 1366×768, and 390×844, compare against DESIGN.md, and fix mismatches.
6. Stop only for a real blocker you cannot work around, and tell me exactly what to do.
7. When finished, write the final report at the end of docs/PROGRESS.md: what works, how each part was verified, known issues, and the exact commands to run the demo.
```

## 3. If the run stops early

Sessions can end before the work does. Start a new session in the same folder and paste:

```
Continue building NexusSync. Read CLAUDE.md and docs/PROGRESS.md, check the last commits, re-run the tests for the last completed step to confirm it still passes, then continue from the first unticked item.
```

## 4. After the run (humans)

1. **Read the final report** in `docs/PROGRESS.md`, especially "Known issues" and "Open problems".
2. **Run it yourself:**
   ```bash
   make -C server asan test
   python3 scripts/crash_test.py 50
   (cd web && npm test && npm run e2e)
   python3 scripts/demo_reset.py
   ```
3. **Click through every screen** in a browser at your laptop's resolution and on the projector. Note anything that looks off against `DESIGN.md`.
4. **Test on real laptops and your own router** following `docs/DEMO.md`. This is the step most likely to find problems the run could not, such as firewalls, Wi-Fi isolation, WSL networking, and real upload speeds.
5. **Collect real lab data** and run `python3 eval/run_eval.py eval/datasets` for the report's results.
6. **Study the code.** Split the modules between teammates and read `docs/LEARNING.md` together. Each person should be able to explain their modules, the write-ahead log and recovery, and the lock order in the viva.
7. **Come back with fixes** as specific Claude Code requests, one problem at a time, quoting the screen or test and what you expected.
