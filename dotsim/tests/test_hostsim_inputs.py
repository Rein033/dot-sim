"""The host-sim's inputs from the page: battery, USB, motion and the console.

Builds apps/slang once for ESP-IDF's linux target (a few minutes the first
time; the build dir under /tmp is kept between runs), starts the ELF against a
unix socket and plays the server's part: sends text lines, reads the sim's
records. Skipped where ESP-IDF is not installed (it lives in the sim CT).
"""
import os
import select
import socket
import struct
import subprocess
import tempfile
import time
from pathlib import Path

import pytest

DOTSIM = Path(__file__).resolve().parents[1]
IDF = Path(os.environ.get("IDF_PATH", "/opt/esp/esp-idf"))
BUILD_DIR = Path(os.environ.get("DOTSIM_TEST_BUILD", "/tmp/dotsim-test-build")) / "slang"

pytestmark = pytest.mark.skipif(not IDF.exists(), reason="ESP-IDF is not installed here")


@pytest.fixture(scope="module")
def elf():
    subprocess.run(["python3", "assemble.py", "slang"], cwd=DOTSIM / "apps", check=True)
    r = subprocess.run(
        [str(DOTSIM / "hostsim" / "build.sh"), str(DOTSIM / "apps" / "slang"), str(BUILD_DIR)],
        capture_output=True, text=True, timeout=1800)
    out = BUILD_DIR / "kode_sim_app.elf"
    assert r.returncode == 0 and out.exists(), (r.stdout + r.stderr)[-3000:]
    return out


class Sim:
    """One running sim: its socket, the records it sent, and a way to send lines."""

    def __init__(self, elf):
        self.dir = tempfile.mkdtemp(prefix="dsim")
        path = os.path.join(self.dir, "s")
        srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        srv.bind(path)
        srv.listen(1)
        srv.settimeout(30)
        env = dict(os.environ, KODE_SIM_SOCK=path, KODE_SIM_DATA=os.path.join(self.dir, "data"))
        self.proc = subprocess.Popen([str(elf)], env=env, cwd=self.dir,
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.conn, _ = srv.accept()
        srv.close()
        self.buf = b""
        self.logs = []

    def send(self, line):
        self.conn.sendall(line.encode() + b"\n")

    def _pump(self, timeout):
        r, _, _ = select.select([self.conn], [], [], timeout)
        if r:
            d = self.conn.recv(1 << 20)
            self.buf += d
        while len(self.buf) >= 5:
            typ = self.buf[0:1]
            (n,) = struct.unpack_from("<I", self.buf, 1)
            if len(self.buf) < 5 + n:
                break
            payload = self.buf[5:5 + n]
            self.buf = self.buf[5 + n:]
            if typ == b"L":
                self.logs.append(payload.decode(errors="replace"))

    def wait_log(self, pred, timeout=10.0, since=0):
        """Wait for an L record (after the first `since`) that pred accepts; returns its text."""
        end = time.time() + timeout
        seen = since
        while True:
            for text in self.logs[seen:]:
                if pred(text):
                    return text
            seen = len(self.logs)
            left = end - time.time()
            if left <= 0:
                raise AssertionError("no matching L record; got %r" % self.logs[-15:])
            self._pump(min(left, 0.5))

    def ask(self, what, prefix):
        """Send '? what' until a record starting with prefix comes (the app may still be booting)."""
        end = time.time() + 10
        while time.time() < end:
            since = len(self.logs)
            self.send("? " + what)
            try:
                return self.wait_log(lambda t: t.strip().startswith(prefix), 1.0, since).strip()
            except AssertionError:
                pass
        raise AssertionError("no %r answer" % prefix)

    def close(self):
        self.proc.kill()
        self.proc.wait()
        self.conn.close()


@pytest.fixture()
def sim(elf):
    s = Sim(elf)
    yield s
    s.close()


def test_battery_line_sets_gauge(sim):
    sim.send("p 15 0")
    assert sim.ask("bat", "bat ") == "bat 15 0"
    sim.send("p 100 1")
    assert sim.ask("bat", "bat ") == "bat 100 1"


def test_motion_line_sets_imu(sim):
    sim.send("i 100 -200 1000 5000 -6000 7000 300 -400 500 253")
    assert sim.ask("imu", "imu ") == "imu 100 -200 1000 5000 -6000 7000 300 -400 500 253"


def test_console_roundtrip(sim):
    sim.send("c help")
    sim.wait_log(lambda t: "help" in t and t.lstrip().startswith(">"))
