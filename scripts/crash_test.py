#!/usr/bin/env python3
"""Crash test against the real server (SPEC §15 "Crash", §14 M7 and M13).

Each round: start nexusd (which recovers from the previous kill), check the
store against a model of everything that was committed, then run random
uploads and deletes from several nexus CLI processes and hard-kill the
server at a random moment between 0.1 and 3 s (SIGKILL on POSIX,
TerminateProcess on Windows). After the restart:
  - every file whose upload had succeeded downloads with its SHA-256,
  - every successful delete stayed deleted,
  - an operation that was in flight at the kill either happened completely
    or not at all (I4), and nothing else appears,
  - after a clean stop, `nexusd --fsck` passes (I1-I3).

Since M12 an admin also starts cleanup every few hundred milliseconds, with
gc_grace_s = 0 and 1 MB containers, so kills land in the middle of freeing
and compaction too (SPEC §10, D-048). --no-cleanup runs the M7 version.

    python3 scripts/crash_test.py 50 [--workers 3] [--seed 1] [--keep] [--no-cleanup]
"""
from __future__ import annotations

import argparse
import random
import shutil
import tempfile
import threading
import time
from pathlib import Path

import nxlib

MAX_FILES = 40  # keep each round's verification quick


class Model:
    """What the clients know was committed."""

    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.files: dict[int, tuple[str, Path, int]] = {}  # id -> (sha, local path, client)
        self.pending_puts: list[tuple[str, Path, int]] = []  # (name, path, client) in flight at a kill
        self.pending_rms: list[int] = []
        self.busy: set[int] = set()  # ids a worker is deleting right now
        self.next_name = 0

    def name(self) -> str:
        with self.lock:
            self.next_name += 1
            return f"f{self.next_name:05d}.csv"


def cleaner(srv: nxlib.Server, cookie: str, rng: random.Random, killed: threading.Event,
            started: list[int], errors: list[str]) -> None:
    """Starts cleanup again and again until the kill (202, or 409 while one runs)."""
    r = random.Random(rng.random())
    while not killed.is_set():
        time.sleep(r.uniform(0.05, 0.4))
        status = srv.admin_post("/api/admin/gc", cookie)
        if status == 202:
            started[0] += 1
        elif status not in (409, None) and not killed.is_set():
            errors.append(f"POST /api/admin/gc answered {status}")


def work_round(clis: list[nxlib.Cli], model: Model, files: Path, rng: random.Random,
               workers: int, killed: threading.Event, errors: list[str]) -> int:
    """Random puts and deletes until the server is killed. Returns ops started."""
    count = [0]

    def worker(k: int) -> None:
        r = random.Random(rng.random())
        while not killed.is_set():
            ci = r.randrange(len(clis))
            cli = clis[ci]
            with model.lock:
                mine = [i for i, (_, _, c) in model.files.items() if c == ci and i not in model.busy]
                n = len(model.files)
            count[0] += 1
            if mine and (n >= MAX_FILES or r.random() < 0.3):
                fid = r.choice(mine)
                with model.lock:  # claim it, so two workers never delete one file
                    if fid not in model.files or fid in model.busy:
                        continue
                    model.busy.add(fid)
                ok = cli.rm(fid)
                with model.lock:
                    model.busy.discard(fid)
                    if ok:
                        model.files.pop(fid, None)
                    elif killed.is_set():
                        model.pending_rms.append(fid)
                    else:
                        errors.append(f"rm {fid} failed while the server was up")
                continue
            # Upload: fresh data, or an edited copy of a file we already have.
            with model.lock:
                srcs = [p for (_, p, _) in model.files.values()]
            if srcs and r.random() < 0.6:
                data = nxlib.edit(r, r.choice(srcs).read_bytes())
            else:
                data = nxlib.csv_rows(r, r.randrange(500, 15000))
            name = model.name()
            p = files / name
            p.write_bytes(data)
            sha = nxlib.sha256_file(p)
            f = cli.put(p)
            with model.lock:
                if f is not None:
                    if f["sha256"] != sha:
                        errors.append(f"server reports a different SHA-256 for {name}")
                    model.files[f["id"]] = (sha, p, ci)
                elif killed.is_set():
                    model.pending_puts.append((name, p, ci))
                else:
                    errors.append(f"put {name} failed while the server was up")

    threads = [threading.Thread(target=worker, args=(k,)) for k in range(workers)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    return count[0]


def verify(clis: list[nxlib.Cli], model: Model, files: Path, errors: list[str]) -> tuple[int, int]:
    """Resolves in-flight operations, then checks the server against the model."""
    listed: dict[int, dict] = {}
    for ci, cli in enumerate(clis):
        for f in cli.ls():
            listed[f["id"]] = dict(f, client=ci)
    resolved_in = resolved_out = 0
    # An upload in flight at the kill: committed completely, or not at all.
    for name, p, ci in model.pending_puts:
        hits = [f for f in listed.values() if f["name"] == name and f["client"] == ci]
        if hits:
            f = hits[0]
            if f["sha256"] != nxlib.sha256_file(p):
                errors.append(f"in-flight upload {name} came back with different content")
            model.files[f["id"]] = (f["sha256"], p, ci)
            resolved_in += 1
        else:
            resolved_out += 1
    model.pending_puts.clear()
    for fid in model.pending_rms:
        if fid not in listed:
            model.files.pop(fid, None)
    model.pending_rms.clear()
    # Exactly the committed files, each with the right bytes.
    extra = set(listed) - set(model.files)
    missing = set(model.files) - set(listed)
    if extra:
        errors.append(f"files nobody committed: {sorted(extra)}")
    if missing:
        errors.append(f"committed files lost: {sorted(missing)}")
    for fid, (sha, p, ci) in model.files.items():
        out = files / f"dl-{fid}"
        if not clis[ci].get(fid, out):
            errors.append(f"download of file {fid} failed")
        elif nxlib.sha256_file(out) != sha:
            errors.append(f"file {fid} downloaded with the wrong SHA-256")
        out.unlink(missing_ok=True)
    return resolved_in, resolved_out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("rounds", type=int, nargs="?", default=20)
    ap.add_argument("--workers", type=int, default=3)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--keep", action="store_true", help="keep the work directory")
    ap.add_argument("--no-cleanup", action="store_true", help="no cleanup runs during the rounds (M7)")
    args = ap.parse_args()
    cleanup = not args.no_cleanup

    nxlib.build()
    work = Path(tempfile.mkdtemp(prefix="nexussync-crash-"))
    files = work / "files"
    files.mkdir()
    srv = nxlib.Server(work / "server",
                       extra_conf="gc_grace_s = 0\ncontainer_size = 1048576\n" if cleanup else "")
    rng = random.Random(args.seed)
    model = Model()
    errors: list[str] = []
    ops = inflight_in = inflight_out = 0
    gc_started = [0]
    try:
        srv.init()
        srv.start()
        clis = []
        for name in ("riya", "kabir"):
            srv.join(name, "long-enough-1")
            cli = nxlib.Cli(srv, work / f"home-{name}", name, "long-enough-1")
            cli.login()
            clis.append(cli)
        # Signed in once: sessions survive a kill (D-030), and signing in
        # inside a round could race the killer.
        cookie = srv.admin_cookie() if cleanup else ""
        for rnd in range(1, args.rounds + 1):
            killed = threading.Event()
            delay = rng.uniform(0.1, 3.0)

            def killer() -> None:
                time.sleep(delay)
                killed.set()
                srv.kill()

            kt = threading.Thread(target=killer)
            kt.start()
            ct = None
            if cleanup:
                ct = threading.Thread(target=cleaner, args=(srv, cookie, rng, killed, gc_started, errors))
                ct.start()
            ops += work_round(clis, model, files, rng, args.workers, killed, errors)
            kt.join()
            if ct:
                ct.join()
            srv.start()  # recovery runs here
            a, b = verify(clis, model, files, errors)
            inflight_in += a
            inflight_out += b
            srv.stop()
            r = srv.fsck()
            if r.returncode != 0:
                errors.append(f"round {rnd}: fsck failed:\n{r.stdout}{r.stderr}")
            nxlib.say(f"round {rnd:3d}: killed after {delay:.2f} s; {len(model.files)} files committed; "
                      f"fsck {'clean' if r.returncode == 0 else 'FAILED'}; errors so far {len(errors)}")
            if errors:
                break
            srv.start()
    except Exception as e:  # noqa: BLE001 - report anything as a failure
        errors.append(f"{type(e).__name__}: {e}")
    finally:
        srv.stop()
    for e in errors:
        nxlib.fail(e)
    nxlib.say(f"{args.rounds} rounds, {ops} operations; uploads in flight at a kill: "
              f"{inflight_in} committed, {inflight_out} not; {len(model.files)} files at the end")
    if cleanup:
        log = (work / "server" / "server.log").read_text(errors="replace")
        done = log.count("cleanup done ")
        compacting = sum(1 for ln in log.splitlines()
                         if "cleanup done " in ln and "containers_compacted=0 " not in ln)
        nxlib.say(f"cleanup: {gc_started[0]} runs started, {done} finished before a kill "
                  f"({compacting} of them compacted containers); the rest were cut short by a kill")
    if args.keep or errors:
        nxlib.say(f"work directory kept: {work}")
    else:
        shutil.rmtree(work, ignore_errors=True)
    nxlib.say("crash test: " + ("PASSED" if not errors else "FAILED"))
    return 0 if not errors else 1


if __name__ == "__main__":
    raise SystemExit(main())
