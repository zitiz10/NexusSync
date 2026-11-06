#!/usr/bin/env python3
"""Wipes the demo server and starts it fresh (docs/DEMO.md §3.1, §5).

    python3 scripts/demo_reset.py [--config nexussync.conf] [--yes] [--detach]

1. Refuses if a server already answers on the configured port (stop it first).
2. Deletes data_dir, after asking (--yes skips the question). It only
   deletes a directory that holds a NexusSync superblock.
3. nexusd --init, then the admin account and the two students riya and
   arjun. Passwords come from demo.env next to the config file; if there is
   none, one is written with new random passwords (mode 0600, git-ignored).
   Passwords are never printed: read them from demo.env.
4. Starts nexusd in this terminal (Ctrl+C stops it cleanly). With --detach
   it keeps running in the background and its pid is printed.

The store starts empty, as the demo script expects ("Nothing stored yet").
"""
from __future__ import annotations

import argparse
import json
import os
import secrets
import shutil
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXE = ".exe" if os.name == "nt" else ""
NEXUSD = ROOT / "server" / "build" / f"nexusd{EXE}"
PEOPLE = (("admin", "Lab Admin", "ADMIN_PASSWORD"),
          ("riya", "Riya Sharma", "RIYA_PASSWORD"),
          ("arjun", "Arjun Mehta", "ARJUN_PASSWORD"))
ALPHABET = "23456789abcdefghjkmnpqrstuvwxyz"  # no 0/o, 1/l/i: easy to type from a sheet


def read_conf(path: Path) -> dict[str, str]:
    conf = {}
    for line in path.read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if "=" in line:
            k, v = line.split("=", 1)
            conf[k.strip()] = v.strip()
    return conf


def load_env(path: Path) -> dict[str, str]:
    env: dict[str, str] = {}
    if path.exists():
        for line in path.read_text().splitlines():
            if "=" in line and not line.lstrip().startswith("#"):
                k, v = line.split("=", 1)
                env[k.strip()] = v.strip()
    missing = [key for _, _, key in PEOPLE if len(env.get(key, "")) < 10]
    if missing:
        for key in missing:
            env[key] = "".join(secrets.choice(ALPHABET) for _ in range(14))
        text = "# Demo passwords for demo_reset.py (git-ignored). Keep this file private.\n"
        text += "".join(f"{k}={env[k]}\n" for _, _, k in PEOPLE)
        path.write_text(text)
        try:
            path.chmod(0o600)
        except OSError:
            pass  # Windows: no POSIX modes
        print(f"New passwords written to {path} (not shown here).")
    return env


def health(url: str) -> str | None:
    try:
        with urllib.request.urlopen(url + "/api/health", timeout=1) as r:
            return json.load(r).get("status")
    except (urllib.error.URLError, ConnectionError, TimeoutError, OSError, ValueError):
        return None


def post_json(url: str, path: str, body: dict) -> int:
    req = urllib.request.Request(url + path, data=json.dumps(body).encode(), method="POST",
                                 headers={"Content-Type": "application/json", "X-NexusSync": "1"})
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return r.status
    except urllib.error.HTTPError as e:
        return e.code


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--config", type=Path, default=ROOT / "nexussync.conf")
    ap.add_argument("--yes", action="store_true", help="do not ask before deleting data_dir")
    ap.add_argument("--detach", action="store_true", help="leave nexusd running in the background")
    args = ap.parse_args()
    t0 = time.monotonic()

    conf_path: Path = args.config.resolve()
    if not conf_path.exists():
        print(f"No config at {conf_path}. Start from the example: cp nexussync.conf.example nexussync.conf")
        return 2
    if not NEXUSD.exists():
        print(f"{NEXUSD} is missing. Build it first: make -C server")
        return 2
    conf = read_conf(conf_path)
    # nexusd resolves relative paths from the directory it starts in: the
    # config file's directory, as in DEMO.md §3.1.
    cwd = conf_path.parent
    data_dir = (cwd / conf.get("data_dir", "./data")).resolve()
    port = conf.get("listen", "0.0.0.0:8080").rsplit(":", 1)[-1]
    local = f"http://127.0.0.1:{port}"

    if health(local):
        print(f"A server is already running on port {port}. Stop it first (Ctrl+C in its terminal), "
              "then run this again.")
        return 1
    if data_dir.exists():
        if not ((data_dir / "superblock.a").exists() or (data_dir / "superblock.b").exists()):
            print(f"{data_dir} exists but is not a NexusSync data directory; refusing to delete it.")
            return 1
        if not args.yes:
            answer = input(f"Delete {data_dir} and every file stored in it? [y/N] ")
            if answer.strip().lower() not in ("y", "yes"):
                print("Nothing changed.")
                return 1
        shutil.rmtree(data_dir)

    env = load_env(cwd / "demo.env")

    def nexusd(*a: str, stdin: str | None = None) -> None:
        r = subprocess.run([str(NEXUSD), "--config", str(conf_path), *a], cwd=cwd, input=stdin,
                           capture_output=True, text=True)
        if r.returncode != 0:
            raise SystemExit(f"nexusd {' '.join(a)} failed:\n{r.stdout}{r.stderr}")

    nexusd("--init")
    user, display, key = PEOPLE[0]
    nexusd("--create-admin", stdin=f"{user}\n{display}\n{env[key]}\n{env[key]}\n")
    join_code = (data_dir / "join_code").read_text().strip()

    # Detached, the server must not hold this terminal's output: it logs to a file.
    log = open(cwd / "nexusd.log", "a") if args.detach else None
    proc = subprocess.Popen([str(NEXUSD), "--config", str(conf_path)], cwd=cwd, stdout=log, stderr=log,
                            stdin=subprocess.DEVNULL if args.detach else None,
                            start_new_session=args.detach)
    deadline = time.monotonic() + 30
    while health(local) != "ok":
        if proc.poll() is not None or time.monotonic() > deadline:
            print("nexusd did not start; see its output above.")
            return 1
        time.sleep(0.1)
    for user, display, key in PEOPLE[1:]:
        status = post_json(local, "/api/auth/join",
                           {"username": user, "displayName": display, "password": env[key], "joinCode": join_code})
        if status != 201:
            print(f"Could not create {user} (HTTP {status}).")
            proc.terminate()
            return 1
    print(f"\nDemo server ready in {time.monotonic() - t0:.1f} s: empty store, accounts "
          f"{', '.join(p[0] for p in PEOPLE)} (passwords in {cwd / 'demo.env'}).")
    print(f"Join code for more students: {join_code}. Open http://<this laptop's address>:{port}; "
          "the Engine screen shows the addresses with QR codes.")
    if args.detach:
        print(f"nexusd keeps running in the background, pid {proc.pid}; its log is {cwd / 'nexusd.log'}.")
        return 0
    print("Press Ctrl+C to stop the server.\n")
    try:
        return proc.wait()
    except KeyboardInterrupt:
        # Ctrl+C reaches nexusd too (same process group); wait for its clean stop.
        try:
            return proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            proc.send_signal(signal.SIGTERM)
            return proc.wait()


if __name__ == "__main__":
    sys.exit(main())
