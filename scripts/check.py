#!/usr/bin/env python3
"""Run every test suite: C server tests (under sanitizers where available),
the real-process concurrency test (8 CLI clients against a real nexusd),
and the web unit tests. Exit code 0 only if all pass.

Works on Linux, macOS, and Windows (run from an MSYS2 UCRT64 shell so
`make` and gcc are on PATH).

    python3 scripts/check.py            # server + concurrency + web
    python3 scripts/check.py --server   # only the C tests
    python3 scripts/check.py --web      # only the web tests

The crash test is separate because it takes minutes:
    python3 scripts/crash_test.py 50
"""
import argparse
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def run(label: str, cmd: list[str], cwd: Path) -> bool:
    exe = shutil.which(cmd[0])
    if exe is None:
        print(f"[{label}] cannot find '{cmd[0]}' on PATH", file=sys.stderr)
        return False
    print(f"[{label}] {' '.join(cmd)}  (in {cwd.relative_to(ROOT) or '.'})", flush=True)
    t0 = time.monotonic()
    # Use the resolved path so Windows finds npm.cmd as well as npm.
    rc = subprocess.call([exe, *cmd[1:]], cwd=cwd)
    secs = time.monotonic() - t0
    status = "passed" if rc == 0 else f"FAILED (exit {rc})"
    print(f"[{label}] {status} in {secs:.1f} s", flush=True)
    return rc == 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--server", action="store_true", help="run only the C tests")
    ap.add_argument("--web", action="store_true", help="run only the web tests")
    args = ap.parse_args()
    do_server = args.server or not args.web
    do_web = args.web or not args.server

    results = []
    if do_server:
        results.append(("server", run("server", ["make", "-C", "server", "test"], ROOT)))
    if not args.server and not args.web:
        results.append(("concurrency", run("concurrency", [sys.executable, "scripts/concurrency_test.py"], ROOT)))
    if do_web:
        web = ROOT / "web"
        if not (web / "node_modules").is_dir():
            results.append(("web install", run("web", ["npm", "ci"], web)))
        results.append(("web", run("web", ["npm", "test"], web)))

    print()
    for name, ok in results:
        print(f"  {'ok    ' if ok else 'FAILED'} {name}")
    return 0 if all(ok for _, ok in results) else 1


if __name__ == "__main__":
    sys.exit(main())
