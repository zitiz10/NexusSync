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

    python3 scripts/crash_test.py 50 [--workers 3] [--seed 1] [--keep]
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
    args = ap.parse_args()

    nxlib.build()
    work = Path(tempfile.mkdtemp(prefix="nexussync-crash-"))
    files = work / "files"
    files.mkdir()
    srv = nxlib.Server(work / "server")
    rng = random.Random(args.seed)
    model = Model()
    errors: list[str] = []
    ops = inflight_in = inflight_out = 0
    try:
        srv.init()
        srv.start()
        clis = []
        for name in ("riya", "kabir"):
            srv.join(name, "long-enough-1")
            cli = nxlib.Cli(srv, work / f"home-{name}", name, "long-enough-1")
            cli.login()
            clis.append(cli)
        for rnd in range(1, args.rounds + 1):
            killed = threading.Event()
            delay = rng.uniform(0.1, 3.0)

            def killer() -> None:
                time.sleep(delay)
                killed.set()
                srv.kill()

            kt = threading.Thread(target=killer)
            kt.start()
            ops += work_round(clis, model, files, rng, args.workers, killed, errors)
            kt.join()
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
    if args.keep or errors:
        nxlib.say(f"work directory kept: {work}")
    else:
        shutil.rmtree(work, ignore_errors=True)
    nxlib.say("crash test: " + ("PASSED" if not errors else "FAILED"))
    return 0 if not errors else 1


if __name__ == "__main__":
    raise SystemExit(main())
