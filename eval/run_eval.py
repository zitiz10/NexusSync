#!/usr/bin/env python3
"""NexusSync evaluation (SPEC §16): baselines, network savings, throughput,
recovery and checkpoint times, chunk sizes. Tables (CSV + summary.md) and
charts (PNG + SVG, needs matplotlib) go to the output folder.

    python3 eval/run_eval.py eval/datasets [--out eval/results] [--runs 3]

The dataset folder: each subfolder is a category ("csv", "manuals", ...).
Files directly in it are grouped by extension. Files are processed in name
order, so "x.csv" is uploaded before its edited "x_v2.csv".

What is measured, and how:
  storage   plain size, gzip -6 per file, whole-file dedup (SHA-256 of the
            file), fixed 4 KB block dedup, NexusSync CDC dedup. The block
            baselines use the server's own chunkers (server/build/tests/
            tools/blockhash), so they are exactly what the server stores.
  network   a fresh nexusd on 127.0.0.1; one user uploads every file with
            the nexus CLI (bytes sent are the server's bytesSent); a second
            user then uploads the same files (an identical copy); every file
            is downloaded and its SHA-256 checked. Repeated --runs times on a
            fresh server; throughput is the mean. Localhost has no network
            limit, so throughput here measures the software, not a LAN.
  check     the server's physical bytes after the first pass must equal the
            CDC baseline's unique bytes, or the run fails.
  recovery  in run 1, the server is hard-killed after each first-pass upload
            and restarted; the log records replayed and the recovery time
            come from the server log (the WAL file size is listed too, but a
            segment file only grows, so it is not the amount replayed).
  checkpoint after each second-pass upload, an admin checkpoint: pages and ms.
  chunks    the CDC block size distribution over all files.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import os
import platform
import re
import statistics
import subprocess
import sys
import tempfile
import time
import urllib.request
import json
import zlib
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
import nxlib  # noqa: E402

BLOCKHASH = nxlib.BUILD / "tests" / "tools" / f"blockhash{nxlib.EXE}"
METHODS = ("plain", "gzip", "whole-file", "fixed 4 KB", "CDC (NexusSync)")
EXT_CATEGORY = {".csv": "CSV data", ".tsv": "CSV data", ".txt": "text", ".md": "text", ".tar": "source tar",
                ".zip": "zip-based", ".docx": "zip-based", ".xlsx": "zip-based", ".pptx": "zip-based",
                ".gz": "compressed (one stream)", ".pdf": "PDF", ".iso": "disk images", ".img": "disk images"}


def mb(n: float) -> str:
    """1024-based, like the web app and the demo data ("300.0 MB")."""
    return f"{n / 2**20:,.1f} MB" if n >= 100 * 1024 else f"{n / 1024:,.1f} KB"


# ---- dataset ------------------------------------------------------------------------------

def collect(folder: Path) -> list[tuple[str, Path]]:
    items = []
    for p in sorted(folder.iterdir()):
        if p.name.startswith("."):
            continue
        if p.is_dir():
            items += [(p.name, f) for f in sorted(p.rglob("*")) if f.is_file() and not f.name.startswith(".")]
        elif p.is_file():
            items.append((EXT_CATEGORY.get(p.suffix.lower(), "other"), p))
    return items


def blocks(kind: str, path: Path) -> list[tuple[int, str]]:
    r = subprocess.run([str(BLOCKHASH), kind, str(path)], capture_output=True, text=True, check=True)
    return [(int(a), b) for a, b in (line.split() for line in r.stdout.splitlines())]


def gzip_size(path: Path) -> int:
    z = zlib.compressobj(6, zlib.DEFLATED, 31)  # gzip container, level 6
    n = 0
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            n += len(z.compress(chunk))
    return n + len(z.flush())


def storage(items: list[tuple[str, Path]]):
    """Per category and overall: bytes each method would store."""
    per = defaultdict(lambda: dict.fromkeys(METHODS, 0))
    seen = {m: defaultdict(set) for m in ("whole-file", "fixed 4 KB", "CDC (NexusSync)")}
    glob = {m: set() for m in seen}
    total = dict.fromkeys(METHODS, 0)
    sizes: list[int] = []
    t_chunk = 0.0
    for cat, p in items:
        size = p.stat().st_size
        per[cat]["plain"] += size
        total["plain"] += size
        g = gzip_size(p)
        per[cat]["gzip"] += g
        total["gzip"] += g
        h = nxlib.sha256_file(p)
        for key, method in ((h, "whole-file"),):
            if key not in seen[method][cat]:
                seen[method][cat].add(key)
                per[cat][method] += size
            if key not in glob[method]:
                glob[method].add(key)
                total[method] += size
        for kind, method in (("fixed", "fixed 4 KB"), ("cdc", "CDC (NexusSync)")):
            c0, w0 = os.times().children_user, time.perf_counter()
            bl = blocks(kind, p)
            if kind == "cdc":
                # CPU time of the chunker process; Windows reports none, so wall time there.
                cpu = os.times().children_user - c0
                t_chunk += cpu if cpu > 0 else time.perf_counter() - w0
                sizes += [n for n, _ in bl]
            for n, hh in bl:
                if hh not in seen[method][cat]:
                    seen[method][cat].add(hh)
                    per[cat][method] += n
                if hh not in glob[method]:
                    glob[method].add(hh)
                    total[method] += n
    return dict(per), total, sizes, t_chunk


# ---- server runs --------------------------------------------------------------------------------

def wal_bytes(srv: nxlib.Server) -> int:
    return sum(f.stat().st_size for f in (srv.data / "wal").iterdir() if f.is_file())


RECOVERY = re.compile(r"engine recovery records_replayed=(\d+) txns_redone=(\d+).* ms=(\d+)")


def server_run(items, work: Path, run: int, with_recovery: bool):
    srv = nxlib.Server(work / f"server{run}", extra_conf="checkpoint_interval_s = 3600\n")
    srv.init()
    srv.start()
    admin = nxlib.Cli(srv, work / f"home-admin{run}", "admin", srv.admin_password)
    admin.login()
    srv.join("student", "long-enough-1")
    student = nxlib.Cli(srv, work / f"home-student{run}", "student", "long-enough-1")
    student.login()
    cookie = srv.admin_cookie()
    uploads, recoveries, checkpoints, errors = [], [], [], []
    first: list[tuple[dict, Path]] = []
    for cat, p in items:
        t = time.perf_counter()
        f = admin.put(p)
        dt = time.perf_counter() - t
        if f is None:
            errors.append(f"upload of {p.name} failed")
            continue
        first.append((f, p))
        uploads.append({"run": run, "pass": "first", "category": cat, "file": p.name, "size": f["size"],
                        "bytes_sent": f["stats"]["bytesSent"], "blocks": f["stats"]["chunks"],
                        "new_blocks": f["stats"]["newChunks"], "seconds": dt})
        if with_recovery:
            w = wal_bytes(srv)
            srv.kill()
            log_before = (srv.work / "server.log").read_text(errors="replace")
            srv.start()
            log = (srv.work / "server.log").read_text(errors="replace")[len(log_before):]
            m = RECOVERY.search(log)
            if m:
                recoveries.append({"after": p.name, "wal_bytes": w, "records_replayed": int(m[1]),
                                   "transactions_redone": int(m[2]), "recovery_ms": int(m[3])})
    st = admin.stats()
    for cat, p in items:
        t = time.perf_counter()
        f = student.put(p)
        dt = time.perf_counter() - t
        if f is None:
            errors.append(f"second upload of {p.name} failed")
            continue
        uploads.append({"run": run, "pass": "identical copy", "category": cat, "file": p.name,
                        "size": f["size"], "bytes_sent": f["stats"]["bytesSent"], "blocks": f["stats"]["chunks"],
                        "new_blocks": f["stats"]["newChunks"], "seconds": dt})
        req = urllib.request.Request(srv.url + "/api/admin/checkpoint", data=b"", method="POST",
                                           headers={"Cookie": cookie, "X-NexusSync": "1"})
        with urllib.request.urlopen(req, timeout=60) as r:
            c = json.load(r)
        checkpoints.append({"after": p.name, "pages": c["pages"], "ms": c["durationMs"]})
    downloads = []
    for f, p in first:
        out = work / f"dl{run}-{f['id']}"
        t = time.perf_counter()
        ok = admin.get(f["id"], out)
        dt = time.perf_counter() - t
        if not ok or nxlib.sha256_file(out) != nxlib.sha256_file(p):
            errors.append(f"download of {p.name} does not match")
        downloads.append({"run": run, "file": p.name, "size": f["size"], "seconds": dt})
        out.unlink(missing_ok=True)
    srv.stop()
    fsck = srv.fsck()
    if fsck.returncode != 0:
        errors.append("fsck failed:\n" + fsck.stdout)
    return st, uploads, downloads, recoveries, checkpoints, errors


# ---- output --------------------------------------------------------------------------------------------

def write_csv(path: Path, rows: list[dict]) -> None:
    if not rows:
        return
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)


def system_info() -> list[str]:
    mem = "unknown"
    try:
        if sys.platform == "darwin":
            mem = f"{int(subprocess.run(['sysctl', '-n', 'hw.memsize'], capture_output=True, text=True).stdout) / 2**30:.0f} GB"
        elif sys.platform.startswith("linux"):
            kb = int(next(l for l in open("/proc/meminfo") if l.startswith("MemTotal")).split()[1])
            mem = f"{kb / 2**20:.0f} GB"
    except (OSError, ValueError, StopIteration):
        pass
    cpu = platform.processor() or platform.machine()
    if sys.platform == "darwin":
        cpu = subprocess.run(["sysctl", "-n", "machdep.cpu.brand_string"], capture_output=True, text=True).stdout.strip() or cpu
    ver = subprocess.run([str(nxlib.BUILD / f"nexusd{nxlib.EXE}"), "--version"], capture_output=True, text=True).stdout.strip()
    return [f"OS: {platform.platform()}", f"CPU: {cpu}, {os.cpu_count()} logical cores", f"RAM: {mem}",
            f"Python {platform.python_version()}; {ver}"]


def charts(out: Path, per, total, uploads, sizes, recoveries) -> list[str]:
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        return ["charts skipped: matplotlib is not installed (python3 -m pip install --user matplotlib)"]
    # Validated categorical palette (dataviz reference instance, light mode).
    series = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4"]
    ink, ink2, grid, surface = "#0b0b0b", "#52514e", "#e4e3df", "#fcfcfb"
    plt.rcParams.update({"font.size": 10, "axes.edgecolor": grid, "axes.labelcolor": ink2, "xtick.color": ink2,
                         "ytick.color": ink2, "axes.facecolor": surface, "figure.facecolor": surface,
                         "axes.spines.top": False, "axes.spines.right": False, "svg.fonttype": "none"})
    made = []

    def save(fig, name):
        for ext in ("png", "svg"):
            fig.savefig(out / f"{name}.{ext}", dpi=160, bbox_inches="tight")
        plt.close(fig)
        made.append(name)

    def recessive(ax, axis="y"):
        ax.grid(axis=axis, color=grid, linewidth=0.8)
        ax.set_axisbelow(True)
        ax.tick_params(length=0)

    # 1. Deduplication ratio per category and method.
    cats = sorted(per) + ["all data"]
    data = {**per, "all data": total}
    fig, ax = plt.subplots(figsize=(max(7, 1.6 * len(cats) + 2), 4.2))
    shown = METHODS[1:]  # plain is 1.0 by definition: drawn as the reference line
    width = 0.8 / len(shown)
    ax.axhline(1, color=ink2, linewidth=1, linestyle=(0, (3, 3)), zorder=1, label="1× = plain storage")
    for i, m in enumerate(shown):
        xs = [k + (i - (len(shown) - 1) / 2) * width for k in range(len(cats))]
        ys = [data[c]["plain"] / max(data[c][m], 1) for c in cats]
        bars = ax.bar(xs, ys, width * 0.9, color=series[i], label=m)  # 0.9: a surface gap between bars
        for b, y in zip(bars, ys):
            ax.annotate(f"{y:.1f}", (b.get_x() + b.get_width() / 2, y), ha="center", va="bottom",
                        fontsize=7, color=ink2, xytext=(0, 2), textcoords="offset points")
    ax.set_xticks(range(len(cats)), cats)
    ax.set_ylabel("Space saved: plain size ÷ stored size (×)")
    ax.set_title("Storage needed, by method", loc="left", color=ink, fontsize=12)
    ax.legend(frameon=False, ncol=len(shown) + 1, loc="upper left", bbox_to_anchor=(0, -0.1), fontsize=8)
    recessive(ax)
    save(fig, "dedup_ratio")

    # 2. Share of each first upload that crossed the network.
    first = [u for u in uploads if u["run"] == 1 and u["pass"] == "first"]
    if first:
        fig, ax = plt.subplots(figsize=(7, 0.45 * len(first) + 1.4))
        names = [u["file"] for u in first][::-1]
        pct = [100 * u["bytes_sent"] / max(u["size"], 1) for u in first][::-1]
        bars = ax.barh(names, pct, 0.7, color=series[0])
        for b, u in zip(bars, first[::-1]):
            ax.annotate(f"{mb(u['bytes_sent'])} of {mb(u['size'])}", (b.get_width(), b.get_y() + b.get_height() / 2),
                        va="center", fontsize=8, color=ink2, xytext=(4, 0), textcoords="offset points")
        ax.set_xlim(0, 125)
        ax.set_xticks(range(0, 101, 25))
        ax.set_xlabel("Bytes sent, % of file size (first upload, in name order)")
        ax.set_title("What each upload had to send", loc="left", color=ink, fontsize=12)
        recessive(ax, "x")
        save(fig, "network_sent")

    # 3. CDC block sizes.
    if sizes:
        fig, ax = plt.subplots(figsize=(7, 3.6))
        edges = list(range(0, 16385 + 1024, 1024))
        ax.hist(sizes, bins=edges, color=series[0], rwidth=0.92)
        ax.axvline(statistics.mean(sizes), color=ink, linewidth=1, linestyle=(0, (3, 3)))
        ax.annotate(f"mean {statistics.mean(sizes) / 1024:.1f} KB", (statistics.mean(sizes), ax.get_ylim()[1] * 0.92),
                    xytext=(6, 0), textcoords="offset points", fontsize=8, color=ink2)
        ax.set_xticks(range(0, 16385, 2048), [f"{k // 1024}" for k in range(0, 16385, 2048)])
        ax.set_xlabel("Block size (KB): minimum 2, target 4, maximum 16")
        ax.set_ylabel("Blocks")
        ax.set_title("Content-defined block sizes", loc="left", color=ink, fontsize=12)
        recessive(ax)
        save(fig, "chunk_sizes")

    # 4. Recovery time against the log it had to replay.
    if recoveries:
        fig, ax = plt.subplots(figsize=(6, 3.8))
        xs = [r["records_replayed"] for r in recoveries]
        ys = [r["recovery_ms"] for r in recoveries]
        ax.scatter(xs, ys, s=64, color=series[0], edgecolors=surface, linewidths=2, zorder=3)
        ax.set_xlabel("Log records replayed")
        ax.set_ylabel("Recovery time (ms)")
        ax.set_ylim(bottom=0)
        ax.set_xlim(left=0)
        ax.set_title("Recovery time after a hard kill", loc="left", color=ink, fontsize=12)
        recessive(ax, "both")
        save(fig, "recovery")
    return [f"charts: {', '.join(made)} (PNG and SVG)"]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("datasets", type=Path)
    ap.add_argument("--out", type=Path, default=ROOT / "eval" / "results")
    ap.add_argument("--runs", type=int, default=3)
    args = ap.parse_args()
    items = collect(args.datasets)
    if not items:
        print(f"No files in {args.datasets}.")
        return 2
    args.out.mkdir(parents=True, exist_ok=True)
    nxlib.build()
    if not BLOCKHASH.exists():
        print(f"{BLOCKHASH} is missing; run make -C server")
        return 2

    nxlib.say(f"{len(items)} files, {mb(sum(p.stat().st_size for _, p in items))}; storage baselines ...")
    per, total, sizes, t_chunk = storage(items)
    errors: list[str] = []
    uploads, downloads, recoveries, checkpoints = [], [], [], []
    with tempfile.TemporaryDirectory(prefix="nexussync-eval-") as tmp:
        for run in range(1, args.runs + 1):
            nxlib.say(f"server run {run} of {args.runs} ...")
            st, u, d, r, c, e = server_run(items, Path(tmp), run, with_recovery=run == 1)
            uploads += u
            downloads += d
            recoveries += r
            checkpoints += c
            errors += e
            if st["physicalBytes"] != total["CDC (NexusSync)"]:
                errors.append(f"run {run}: server stores {st['physicalBytes']} bytes, the CDC baseline predicts "
                              f"{total['CDC (NexusSync)']}")

    # Tables.
    storage_rows = [{"category": c, **{m: v[m] for m in METHODS}} for c, v in sorted(per.items())]
    storage_rows.append({"category": "all data", **total})
    write_csv(args.out / "storage.csv", storage_rows)
    write_csv(args.out / "uploads.csv", uploads)
    write_csv(args.out / "downloads.csv", downloads)
    write_csv(args.out / "recovery.csv", recoveries)
    write_csv(args.out / "checkpoints.csv", checkpoints)
    hist = Counter(min(s // 1024, 16) for s in sizes)
    write_csv(args.out / "chunk_sizes.csv", [{"kb_from": k, "kb_to": k + 1, "blocks": hist[k]} for k in range(17)])

    lines = ["# NexusSync evaluation results", "", f"Dataset: `{args.datasets}`, {len(items)} files, "
             f"{mb(total['plain'])}. Server runs: {args.runs} (throughput is their mean). "
             "Network numbers are from 127.0.0.1, so they measure the software, not a LAN. "
             "Sizes are 1024-based (1 MB = 1,048,576 bytes).", "",
             *[f"- {s}" for s in system_info()], "", "## Storage by method", "",
             "Stored size, and plain size ÷ stored size in brackets.", "",
             "| Category | " + " | ".join(METHODS) + " |", "|---|" + "---:|" * len(METHODS)]
    for row in storage_rows:
        lines.append(f"| {row['category']} | " + " | ".join(
            f"{mb(row[m])} ({row['plain'] / max(row[m], 1):.2f}×)" for m in METHODS) + " |")
    lines += ["", "## Network: bytes each upload sent (run 1)", "",
              "| File | Size | First upload sent | Identical copy sent | First upload MB/s (mean) |",
              "|---|---:|---:|---:|---:|"]
    for cat, p in items:
        f1 = [u for u in uploads if u["file"] == p.name and u["pass"] == "first"]
        f2 = [u for u in uploads if u["file"] == p.name and u["pass"] == "identical copy"]
        if not f1:
            continue
        rate = statistics.mean(u["size"] / max(u["seconds"], 1e-9) / 2**20 for u in f1)
        lines.append(f"| {p.name} | {mb(f1[0]['size'])} | {mb(f1[0]['bytes_sent'])} | "
                     f"{mb(f2[0]['bytes_sent']) if f2 else '–'} | {rate:,.1f} |")
    if downloads:
        rate = sum(d["size"] for d in downloads) / max(sum(d["seconds"] for d in downloads), 1e-9) / 2**20
        lines += ["", f"Downloads: {len(downloads)}, all SHA-256 checked, {rate:,.1f} MB/s on average."]
    copies = [u for u in uploads if u["pass"] == "identical copy"]
    if copies:
        bps = sum(u["blocks"] for u in copies) / max(sum(u["seconds"] for u in copies), 1e-9)
        lines.append(f"Identical copies: {bps:,.0f} blocks per second end to end (chunking, hashing, "
                     "and one index lookup per block).")
    if sizes:
        lines += ["", "## Chunking", "", f"{len(sizes):,} CDC blocks, mean {statistics.mean(sizes) / 1024:.2f} KB, "
                  f"median {statistics.median(sizes) / 1024:.2f} KB. Chunking plus SHA-256: "
                  f"{total['plain'] / max(t_chunk, 1e-9) / 2**20:,.0f} MB/s of CPU time on one core."]
    if recoveries:
        lines += ["", "## Recovery after a hard kill (run 1)", "", "| After uploading | WAL file size | Records replayed | Recovery |",
                  "|---|---:|---:|---:|"]
        lines += [f"| {r['after']} | {mb(r['wal_bytes'])} | {r['records_replayed']:,} | {r['recovery_ms']} ms |"
                  for r in recoveries]
    if checkpoints:
        ms = [c["ms"] for c in checkpoints]
        lines += ["", f"Checkpoints: {len(ms)}, {statistics.mean(ms):.0f} ms on average, longest {max(ms)} ms, "
                  f"up to {max(c['pages'] for c in checkpoints)} pages."]
    notes = charts(args.out, per, total, uploads, sizes, recoveries)
    lines += ["", *[f"_{n}_" for n in notes]]
    if errors:
        lines += ["", "## ERRORS", "", *[f"- {e}" for e in errors]]
    (args.out / "summary.md").write_text("\n".join(lines) + "\n")
    for n in notes:
        nxlib.say(n)
    for e in errors:
        nxlib.fail(e)
    nxlib.say(f"results in {args.out}: summary.md, *.csv" + ("" if errors else "; all checks passed"))
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
