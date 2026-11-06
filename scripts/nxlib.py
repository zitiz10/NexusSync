"""Helpers for the scripts that drive real nexusd and nexus processes
(concurrency_test.py, crash_test.py). Standard library only, so they run
unchanged on Linux, macOS, and Windows.
"""
from __future__ import annotations

import hashlib
import json
import os
import random
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXE = ".exe" if os.name == "nt" else ""
BUILD = ROOT / "server" / "build"


def build() -> None:
    """Release build of nexusd and nexus (the code under test)."""
    cmd = ["make", "-C", str(ROOT / "server")]
    if os.name == "nt":
        cmd.append("CC=gcc")  # MSYS2 UCRT64, as in CI
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Server:
    """One nexusd on 127.0.0.1 with its own data dir and config."""

    def __init__(self, work: Path, admin_password: str = "admin-password-1", extra_conf: str = ""):
        self.work = work
        self.port = free_port()
        self.url = f"http://127.0.0.1:{self.port}"
        self.conf = work / "nexussync.conf"
        self.data = work / "data"
        self.admin_password = admin_password
        self.proc: subprocess.Popen | None = None
        self.log = None
        work.mkdir(parents=True, exist_ok=True)
        self.conf.write_text(
            f"listen = 127.0.0.1:{self.port}\n"
            f"data_dir = {self.data.as_posix()}\n"
            f"web_root = {(ROOT / 'web' / 'dist').as_posix()}\n"
            "buffer_pool_pages = 256\n"  # small pool: checkpoints happen often
            "checkpoint_interval_s = 2\n" + extra_conf
        )

    def nexusd(self, *args: str, stdin: str | None = None) -> subprocess.CompletedProcess:
        return subprocess.run([str(BUILD / f"nexusd{EXE}"), "--config", str(self.conf), *args],
                              input=stdin, capture_output=True, text=True)

    def init(self) -> None:
        r = self.nexusd("--init")
        assert r.returncode == 0, r.stderr
        pw = self.admin_password
        r = self.nexusd("--create-admin", stdin=f"admin\nLab Admin\n{pw}\n{pw}\n")
        assert r.returncode == 0, r.stdout + r.stderr

    def join_code(self) -> str:
        return (self.data / "join_code").read_text().strip()

    def start(self, timeout: float = 30.0) -> None:
        self.log = open(self.work / "server.log", "a")
        self.proc = subprocess.Popen([str(BUILD / f"nexusd{EXE}"), "--config", str(self.conf)],
                                     stdout=self.log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError(f"nexusd exited with {self.proc.returncode}; see {self.work / 'server.log'}")
            try:
                with urllib.request.urlopen(self.url + "/api/health", timeout=1) as r:
                    if json.load(r).get("status") == "ok":
                        return
            except (urllib.error.URLError, ConnectionError, TimeoutError, OSError):
                pass
            time.sleep(0.05)
        raise RuntimeError("nexusd did not become ready")

    def kill(self) -> None:
        """Hard kill: SIGKILL on POSIX, TerminateProcess on Windows."""
        if self.proc and self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()
        self._close_log()

    def stop(self) -> None:
        """Graceful stop (SIGTERM; on Windows terminate() is a hard kill)."""
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=30)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        self._close_log()

    def _close_log(self) -> None:
        if self.log:
            self.log.close()
            self.log = None

    def fsck(self) -> subprocess.CompletedProcess:
        return self.nexusd("--fsck")

    def join(self, username: str, password: str) -> None:
        body = json.dumps({"username": username, "displayName": username,
                           "password": password, "joinCode": self.join_code()}).encode()
        req = urllib.request.Request(self.url + "/api/auth/join", data=body, method="POST",
                                     headers={"Content-Type": "application/json", "X-NexusSync": "1"})
        with urllib.request.urlopen(req, timeout=30) as r:
            assert r.status == 201


    def admin_cookie(self) -> str:
        """Signs in as the admin; returns the Cookie header value."""
        body = json.dumps({"username": "admin", "password": self.admin_password}).encode()
        req = urllib.request.Request(self.url + "/api/auth/login", data=body, method="POST",
                                     headers={"Content-Type": "application/json", "X-NexusSync": "1"})
        with urllib.request.urlopen(req, timeout=30) as r:
            return r.headers["Set-Cookie"].split(";", 1)[0]

    def admin_post(self, path: str, cookie: str) -> int | None:
        """POST to an admin route; the status, or None if the server is gone."""
        req = urllib.request.Request(self.url + path, data=b"", method="POST",
                                     headers={"Cookie": cookie, "X-NexusSync": "1"})
        try:
            with urllib.request.urlopen(req, timeout=10) as r:
                return r.status
        except urllib.error.HTTPError as e:
            return e.code
        except (urllib.error.URLError, ConnectionError, TimeoutError, OSError):
            return None


class Cli:
    """The nexus CLI for one user, with its own session directory."""

    def __init__(self, server: Server, home: Path, username: str, password: str):
        self.server = server
        self.home = home
        self.username = username
        self.env = dict(os.environ, NEXUS_HOME=str(home), NEXUS_PASSWORD=password)
        home.mkdir(parents=True, exist_ok=True)

    def run(self, *args: str, timeout: float = 300) -> subprocess.CompletedProcess:
        return subprocess.run([str(BUILD / f"nexus{EXE}"), "--server", self.server.url, *args],
                              env=self.env, capture_output=True, text=True, timeout=timeout)

    def login(self) -> None:
        r = self.run("login", self.username)
        assert r.returncode == 0, r.stderr

    def stats(self, as_json: bool = True) -> dict | str:
        """`nexus stats`: the /api/stats reply (--json), or the plain text."""
        r = self.run(*(["--json"] if as_json else []), "stats")
        assert r.returncode == 0, r.stderr
        return json.loads(r.stdout) if as_json else r.stdout

    def put(self, path: Path, *flags: str) -> dict | None:
        """The committed file (dict), or None if the upload failed."""
        r = self.run("--json", "put", str(path), *flags)
        if r.returncode != 0:
            return None
        return json.loads(r.stdout)

    def ls(self) -> list[dict]:
        r = self.run("--json", "ls")
        assert r.returncode == 0, r.stderr
        return json.loads(r.stdout)

    def get(self, file_id: int, out: Path) -> bool:
        return self.run("get", str(file_id), "-o", str(out)).returncode == 0

    def rm(self, file_id: int) -> bool:
        return self.run("rm", str(file_id)).returncode == 0


def sha256_file(p: Path) -> str:
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def csv_rows(rng: random.Random, n: int) -> bytes:
    """Lab-style CSV text: realistic for chunking and deduplication."""
    return "".join(f"{rng.randrange(100000)},{rng.randrange(977)},{rng.random() * 1e4:.3f},"
                   f"sample-{rng.randrange(50)}\n" for _ in range(n)).encode()


def edit(rng: random.Random, data: bytes) -> bytes:
    """A small edit: insert, delete, or overwrite a few bytes somewhere."""
    at = rng.randrange(len(data) + 1)
    k = rng.randrange(1, 400)
    kind = rng.randrange(3)
    if kind == 0:
        return data[:at] + csv_rows(rng, 3)[:k] + data[at:]
    if kind == 1:
        return data[:at] + data[at + k:]
    patch = csv_rows(rng, 3)[:k]
    return data[:at] + patch + data[at + len(patch):]


def say(msg: str) -> None:
    print(msg, flush=True)


def fail(msg: str) -> None:
    print(f"FAIL: {msg}", file=sys.stderr, flush=True)
