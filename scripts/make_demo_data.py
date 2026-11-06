#!/usr/bin/env python3
"""Generates the demo files of docs/DEMO.md §2, deterministically.

    python3 scripts/make_demo_data.py demo-data/            # full size (about 820 MB)
    python3 scripts/make_demo_data.py demo-data/ --scale 0.02  # small, for tests

Every byte comes from a fixed seed, so two runs (on any OS) write identical
files and the demo's numbers repeat. The data is synthetic: it has the
shape of lab data (CSV rows, source files, a manual), not real content.

  dataset.csv / dataset_v2.csv        ~300 MB; v2 has 50 rows inserted near the start
  project_src.tar / project_src_v2.tar ~50 MB; v2 has one source file changed
  lab_manual.txt                       ~5 MB; both students upload it (identical copy)
  submission.zip / submission_v2.zip   the same tree zipped, with one change. Zip
                                       compresses each member on its own, so the
                                       unchanged members still deduplicate
  submission.tar.gz / submission_v2.tar.gz  the same tree as one gzip stream, with the
                                       first file changed: the honest limitation
                                       (one compressed stream hides all repetition
                                       after an edit)
"""
from __future__ import annotations

import argparse
import gzip
import hashlib
import io
import random
import tarfile
import zipfile
from pathlib import Path

MB = 1 << 20
FIXED_TIME = (2026, 1, 15, 9, 0, 0)  # zip entries
FIXED_MTIME = 1768467600             # tar entries: 2026-01-15 09:00 UTC

WORDS = ("the block index maps each fingerprint to a container offset and a reference count "
         "students upload lab data again and again so the server stores each block once "
         "a write ahead log records every change before it reaches the pages on disk "
         "measure the voltage across the resistor and record it in the table below "
         "repeat the experiment three times and report the mean and standard deviation "
         "compile the program with warnings enabled and fix every warning before submission").split()


def csv_rows(rng: random.Random, n: int) -> list[str]:
    rows = []
    for _ in range(n):
        sid = 20240000 + rng.randrange(4000)
        rows.append(f"{sid},CS{rng.randrange(100, 400)},{rng.randrange(1, 15)},"
                    f"{rng.randrange(0, 10001) / 100:.2f},2026-{rng.randrange(1, 13):02d}-"
                    f"{rng.randrange(1, 29):02d}T{rng.randrange(24):02d}:{rng.randrange(60):02d}:00Z,"
                    f"{rng.choice(('ok', 'late', 'resubmitted', 'absent'))}\n")
    return rows


def write_datasets(out: Path, target: int) -> None:
    """dataset.csv, and dataset_v2.csv with 50 rows inserted near the start."""
    rng = random.Random(1)
    header = "student_id,course,week,score,submitted_at,status\n"
    inserted = csv_rows(random.Random(2), 50)
    with open(out / "dataset.csv", "w", newline="\n") as a, open(out / "dataset_v2.csv", "w", newline="\n") as b:
        a.write(header)
        b.write(header)
        written = len(header)
        first = True
        while written < target:
            block = "".join(csv_rows(rng, 2000))
            a.write(block)
            if first:  # the insertion: 50 rows, about 1,000 rows from the start
                cut = block.index("\n", len(block) // 2) + 1
                b.write(block[:cut] + "".join(inserted) + block[cut:])
                first = False
            else:
                b.write(block)
            written += len(block)


def source_tree(rng: random.Random, total: int) -> dict[str, bytes]:
    """A synthetic C project of about `total` bytes."""
    files: dict[str, bytes] = {}
    size = i = 0
    while size < total:
        lines = [f"/* module {i}: generated for the NexusSync demo */", f"#include \"mod{i % 50}.h\"", ""]
        for f in range(rng.randrange(20, 60)):
            lines.append(f"static int f{i}_{f}(int x)\n{{")
            for _ in range(rng.randrange(3, 12)):
                lines.append(f"    x = x * {rng.randrange(2, 97)} + {rng.randrange(1000)}; /* {' '.join(rng.choices(WORDS, k=6))} */")
            lines.append("    return x;\n}\n")
        data = ("\n".join(lines) + "\n").encode()
        files[f"project/src/dir{i % 20:02d}/mod{i:04d}.c"] = data
        size += len(data)
        i += 1
    return files


def edited_tree(files: dict[str, bytes], which: float = 0.5) -> dict[str, bytes]:
    """The same tree with one source file changed: the one `which` of the way through."""
    v2 = dict(files)
    name = sorted(files)[min(int(len(files) * which), len(files) - 1)]
    body = v2[name]
    mid = body.index(b"\n", len(body) // 2) + 1
    v2[name] = body[:mid] + b"    /* fixed after review: check the bounds first */\n" + body[mid:]
    return v2


def write_tar(path, files: dict[str, bytes]) -> None:
    kw = {"fileobj": path} if isinstance(path, io.BytesIO) else {"name": path}
    with tarfile.open(mode="w", format=tarfile.USTAR_FORMAT, **kw) as t:
        for name in sorted(files):
            info = tarfile.TarInfo(name)
            info.size = len(files[name])
            info.mtime = FIXED_MTIME
            info.mode = 0o644
            info.uname = info.gname = "lab"
            t.addfile(info, io.BytesIO(files[name]))


def write_tar_gz(path: Path, files: dict[str, bytes]) -> None:
    """One gzip stream over the tar, with a fixed header time (deterministic)."""
    raw = io.BytesIO()
    write_tar(raw, files)
    with open(path, "wb") as f, gzip.GzipFile(fileobj=f, mode="wb", compresslevel=6, mtime=0, filename="") as g:
        g.write(raw.getvalue())


def write_zip(path: Path, files: dict[str, bytes]) -> None:
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for name in sorted(files):
            z.writestr(zipfile.ZipInfo(name, FIXED_TIME), files[name], compress_type=zipfile.ZIP_DEFLATED)


def write_manual(path: Path, target: int) -> None:
    rng = random.Random(4)
    parts, size, section = [], 0, 1
    while size < target:
        title = f"\n{section}. {' '.join(rng.choices(WORDS, k=4)).capitalize()}\n\n"
        para = "".join(" ".join(rng.choices(WORDS, k=rng.randrange(40, 90))).capitalize() + ".\n\n"
                       for _ in range(rng.randrange(3, 8)))
        parts.append(title + para)
        size += len(title) + len(para)
        section += 1
    path.write_text("".join(parts), newline="\n")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", type=Path)
    ap.add_argument("--scale", type=float, default=1.0, help="size factor (1.0 = DEMO.md sizes)")
    args = ap.parse_args()
    if not 0 < args.scale <= 4:
        ap.error("--scale must be in (0, 4]")
    out: Path = args.out
    out.mkdir(parents=True, exist_ok=True)
    s = args.scale
    write_datasets(out, int(300 * MB * s))
    tree = source_tree(random.Random(3), int(50 * MB * s))
    write_tar(out / "project_src.tar", tree)
    write_tar(out / "project_src_v2.tar", edited_tree(tree))
    write_manual(out / "lab_manual.txt", int(5 * MB * s))
    small = {k: v for k, v in tree.items() if k.startswith("project/src/dir0")}  # a submission-sized subset
    write_zip(out / "submission.zip", small)
    write_zip(out / "submission_v2.zip", edited_tree(small))
    write_tar_gz(out / "submission.tar.gz", small)
    write_tar_gz(out / "submission_v2.tar.gz", edited_tree(small, which=0.0))
    for p in sorted(out.iterdir()):
        h = hashlib.sha256(p.read_bytes()).hexdigest()
        print(f"{p.name:22s} {p.stat().st_size / MB:9.1f} MB  sha256 {h[:16]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
