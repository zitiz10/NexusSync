#!/usr/bin/env python3
"""M7 acceptance: 8 nexus CLI clients upload overlapping files at the same
time against a real nexusd; afterwards every file downloads with a matching
SHA-256 and `nexusd --fsck` is clean (SPEC §14 M7, §15 "Concurrency").

Each client is a separate user and process. All of them upload the same two
base files at the same moment (so many identical new blocks race into the
index), then edited copies, a file of their own, a new version, and a
fixed-size-chunked copy.

    python3 scripts/concurrency_test.py [--clients 8] [--keep]
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


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--clients", type=int, default=8)
    ap.add_argument("--keep", action="store_true", help="keep the work directory")
    args = ap.parse_args()

    nxlib.build()
    work = Path(tempfile.mkdtemp(prefix="nexussync-conc-"))
    srv = nxlib.Server(work / "server")
    ok = True
    try:
        srv.init()
        srv.start()
        rng = random.Random(7)
        base_a = nxlib.csv_rows(rng, 60000)  # ~1.7 MB
        base_b = nxlib.csv_rows(rng, 30000)
        clients = []
        for i in range(args.clients):
            name, pw = f"student{i + 1}", "long-enough-1"
            srv.join(name, pw)
            cli = nxlib.Cli(srv, work / f"home{i}", name, pw)
            cli.login()
            files = work / f"files{i}"
            files.mkdir()
            r = random.Random(100 + i)
            plan = [
                ("A.csv", base_a, []),
                ("B.csv", base_b, []),
                (f"A-edit-{i}.csv", nxlib.edit(r, base_a), []),
                (f"B-edit-{i}.csv", nxlib.edit(r, nxlib.edit(r, base_b)), []),
                (f"own-{i}.csv", nxlib.csv_rows(r, 15000), []),
                ("A.csv", nxlib.edit(r, base_a), []),               # version 2 of A.csv
                (f"A-fixed-{i}.csv", nxlib.edit(r, base_a), ["--fixed"]),
            ]
            clients.append((cli, files, plan))

        results: list[list[tuple[dict | None, Path]]] = [[] for _ in clients]
        go = threading.Barrier(len(clients))

        def worker(k: int) -> None:
            cli, files, plan = clients[k]
            go.wait()  # everyone starts at the same moment
            for step, (name, data, flags) in enumerate(plan):
                d = files / f"{step}"
                d.mkdir()
                p = d / name
                p.write_bytes(data)
                results[k].append((cli.put(p, *flags), p))

        t0 = time.monotonic()
        threads = [threading.Thread(target=worker, args=(k,)) for k in range(len(clients))]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        secs = time.monotonic() - t0

        total = sent = 0
        for k, res in enumerate(results):
            for f, p in res:
                if f is None:
                    nxlib.fail(f"client {k + 1}: upload of {p.name} failed")
                    ok = False
                    continue
                total += f["size"]
                sent += f["stats"]["bytesSent"]
        nxlib.say(f"{len(clients)} clients uploaded {sum(len(r) for r in results)} files "
                  f"({total / 1e6:.1f} MB) in {secs:.1f} s; {sent / 1e6:.1f} MB crossed the network "
                  f"({100 * (1 - sent / max(total, 1)):.1f}% deduplicated)")

        # Every committed file downloads with the SHA-256 of what was uploaded.
        checked = 0
        for k, (cli, files, _) in enumerate(clients):
            listed = {f["id"] for f in cli.ls()}
            for f, p in results[k]:
                if f is None:
                    continue
                out = files / f"dl-{f['id']}"
                if not cli.get(f["id"], out) or nxlib.sha256_file(out) != nxlib.sha256_file(p):
                    nxlib.fail(f"client {k + 1}: download of file {f['id']} ({p.name}) does not match")
                    ok = False
                checked += 1
            # The list shows chain heads: A.csv v1 is replaced by its v2.
            expected = {f["id"] for f, _ in results[k] if f} - {
                f["prevVersionId"] for f, _ in results[k] if f and f["prevVersionId"]}
            if listed != expected:
                nxlib.fail(f"client {k + 1}: ls shows {sorted(listed)}, expected {sorted(expected)}")
                ok = False
        nxlib.say(f"downloaded and verified {checked} files")

        # nexus stats (SPEC §17) reports the same numbers the uploads produced.
        cli0 = clients[0][0]
        st = cli0.stats()
        text = cli0.stats(as_json=False)
        if st["logicalBytes"] != total or st["files"] != sum(1 for r in results for f, _ in r if f):
            nxlib.fail(f"nexus stats: logicalBytes {st['logicalBytes']} files {st['files']}, expected {total}")
            ok = False
        if st["networkSavedBytes"] != total - sent or "Stored files:" not in text or "Your files:" not in text:
            nxlib.fail("nexus stats: network saving or the plain-text lines are wrong:\n" + text)
            ok = False
        nxlib.say("nexus stats: " + text.splitlines()[0].strip())

        srv.stop()
        r = srv.fsck()
        nxlib.say(r.stdout.strip().splitlines()[-1] if r.stdout.strip() else "fsck: no output")
        if r.returncode != 0:
            nxlib.fail("fsck:\n" + r.stdout + r.stderr)
            ok = False
    finally:
        srv.stop()
        if args.keep or not ok:
            nxlib.say(f"work directory kept: {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)
    nxlib.say("concurrency test: " + ("PASSED" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
