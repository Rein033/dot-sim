"""The caps on the sim's sandbox: /tmp 32 MB, /data 16 MB per session, saved data copied in and back (<=1 MB, <=200 files)."""
import os
import shutil
import subprocess
import time

import pytest

import server

needs_bwrap = pytest.mark.skipif(not shutil.which("bwrap"), reason="needs bwrap")


class Sim:
    """A sandbox started with the server's real command line; the 'app' is a shell script."""

    def __init__(self, tmp_path, name, script, saved):
        self.dir = tmp_path / name
        self.dir.mkdir()
        elf = self.dir / "app.elf"
        elf.write_text("#!/bin/sh\n" + script + "\nsleep 60\n")
        elf.chmod(0o755)
        seed = self.dir / "seed"
        server.data_seed(saved, seed)
        (self.dir / "run").mkdir()
        self.p = subprocess.Popen(server.sim_cmd(elf, self.dir / "run", seed),
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.data = self.card = None
        end = time.time() + 10
        while time.time() < end and self.data is None:
            kid = server.sd_child_pid(self.p.pid)
            if kid:
                self.data, self.card = server.data_open(kid), server.sd_open(kid)
            time.sleep(0.05)
        assert self.data is not None and self.card is not None
        self.saved = saved

    def wait_done(self):
        end = time.time() + 15
        while time.time() < end:
            if server.sd_read(self.card, "done")[0] is not None:
                return
            time.sleep(0.1)
        raise AssertionError("app did not finish: " + str(self.p.poll()))

    def res(self, name="res"):
        return (server.sd_read(self.card, name)[0] or b"").decode()

    def end(self):
        ok = server.data_save(self.data, self.saved)
        self.close()
        return ok

    def close(self):
        os.close(self.data)
        os.close(self.card)
        self.p.kill()
        self.p.wait()


@pytest.fixture()
def sims(tmp_path):
    made = []

    def make(script, saved=None, name=None):
        saved = saved or tmp_path / "saved"
        saved.mkdir(exist_ok=True)
        s = Sim(tmp_path, name or "s%d" % len(made), script, saved)
        made.append(s)
        return s

    yield make
    for s in made:
        try:
            s.close()
        except OSError:
            s.p.kill()


@needs_bwrap
def test_50mb_in_tmp_and_data_stops_at_the_caps(sims):
    s = sims("dd if=/dev/zero of=/tmp/big bs=1M count=50 2>/dev/null; wc -c < /tmp/big > /data/sd/res; "
             "dd if=/dev/zero of=/data/big bs=1M count=50 2>/dev/null; wc -c < /data/big >> /data/sd/res; "
             "touch /data/sd/done")
    s.wait_done()
    tmp_size, data_size = [int(x) for x in s.res().split()]
    assert 0 < tmp_size <= 32 * 1024 * 1024
    assert 0 < data_size <= 16 * 1024 * 1024
    assert tmp_size > 16 * 1024 * 1024        # /tmp really has 32 MB, not the card's 16


@needs_bwrap
def test_a_big_data_dir_is_not_saved_and_the_old_data_stays(sims, tmp_path):
    saved = tmp_path / "saved"
    saved.mkdir()
    (saved / "keep").write_text("old")
    s = sims("dd if=/dev/zero of=/data/big bs=1M count=3 2>/dev/null; touch /data/sd/done", saved)
    s.wait_done()
    assert s.end() is False
    assert (saved / "keep").read_text() == "old" and not (saved / "big").exists()
    assert not (tmp_path / "saved.new").exists()


@needs_bwrap
def test_too_many_files_are_not_saved(sims, tmp_path):
    saved = tmp_path / "saved"
    saved.mkdir()
    (saved / "keep").write_text("old")
    s = sims("i=0; while [ $i -lt 250 ]; do : > /data/f$i; i=$((i+1)); done; touch /data/sd/done", saved)
    s.wait_done()
    assert s.end() is False
    assert os.listdir(saved) == ["keep"]


@needs_bwrap
def test_high_score_survives_a_restart(sims, tmp_path):
    saved = tmp_path / "saved"
    s = sims("mkdir -p /data/nvs/kodekit /data/storage; echo 4242 > /data/nvs/kodekit/best_x; echo hi > /data/storage/f; "
             "touch /data/sd/done", saved)
    s.wait_done()
    assert s.end() is True
    t = sims("cat /data/nvs/kodekit/best_x /data/storage/f > /data/sd/res; touch /data/sd/done", saved)
    t.wait_done()
    assert t.res().split() == ["4242", "hi"]


@needs_bwrap
def test_the_card_is_not_part_of_the_saved_data(sims, tmp_path):
    saved = tmp_path / "saved"
    s = sims("echo x > /data/sd/card.txt; echo y > /data/n; touch /data/sd/done", saved)
    s.wait_done()
    assert s.end() is True
    assert sorted(os.listdir(saved)) == ["n"]
    t = sims("ls /data > /data/sd/res; touch /data/sd/done", saved)
    t.wait_done()
    assert "card.txt" not in t.res()


@needs_bwrap
def test_two_sessions_do_not_see_each_others_data(sims, tmp_path):
    saved = tmp_path / "saved"
    a = sims("echo A > /data/from_a; touch /data/sd/done", saved)
    b = sims("sleep 1; ls /data > /data/sd/res; touch /data/sd/done", saved)
    a.wait_done()
    b.wait_done()
    assert "from_a" not in b.res()
    assert not (saved / "from_a").exists()          # nothing is shared before a session ends
    assert a.end() is True
    assert (saved / "from_a").read_text() == "A\n"
    c = sims("ls /data > /data/sd/res; touch /data/sd/done", saved)
    c.wait_done()
    assert "from_a" in c.res()


@needs_bwrap
def test_links_from_the_app_are_not_followed_or_saved(sims, tmp_path):
    saved = tmp_path / "saved"
    s = sims("ln -s /etc/passwd /data/pw; ln -s /etc /data/dir; mkdir /data/real; echo ok > /data/real/f; "
             "touch /data/sd/done", saved)
    s.wait_done()
    assert s.end() is True
    assert sorted(os.listdir(saved)) == ["real"]
    assert (saved / "real" / "f").read_text() == "ok\n"


def test_saved_data_with_a_link_is_not_followed_into_the_seed(tmp_path):
    saved = tmp_path / "saved"
    saved.mkdir()
    secret = tmp_path / "secret"
    secret.write_text("no")
    (saved / "l").symlink_to(secret)
    (saved / "d").symlink_to(tmp_path)
    (saved / "ok").write_text("yes")
    seed = tmp_path / "seed"
    server.data_seed(saved, seed)
    assert os.listdir(seed) == ["ok"]


def test_saved_data_too_big_starts_empty(tmp_path):
    saved = tmp_path / "saved"
    saved.mkdir()
    (saved / "big").write_bytes(b"x" * (server.SAVE_MAX_BYTES + 1))
    seed = tmp_path / "seed"
    server.data_seed(saved, seed)
    assert os.listdir(seed) == []


def test_concurrent_saves_leave_one_complete_tree(tmp_path):
    import threading
    saved = tmp_path / "saved"
    saved.mkdir()
    (saved / "old").write_text("old")
    srcs = []
    for i in range(6):
        d = tmp_path / ("src%d" % i)
        d.mkdir()
        for j in range(20):
            (d / ("f%d_%d" % (i, j))).write_bytes(b"x" * 1000)
        srcs.append(os.open(d, os.O_RDONLY | os.O_DIRECTORY))
    res = []
    ts = [threading.Thread(target=lambda fd=fd: res.append(server.data_save(fd, saved))) for fd in srcs]
    [t.start() for t in ts]
    [t.join() for t in ts]
    [os.close(fd) for fd in srcs]
    assert res == [True] * 6
    names = os.listdir(saved)
    assert len(names) == 20 and len({n.split("_")[0] for n in names}) == 1   # exactly one session's tree
    assert sorted(os.listdir(tmp_path)) == sorted(["saved"] + ["src%d" % i for i in range(6)])   # no staging left


def test_a_data_dir_with_100k_files_is_refused_quickly(tmp_path):
    d = tmp_path / "many"
    d.mkdir()
    for i in range(100000):
        os.close(os.open(d / ("f%d" % i), os.O_CREAT | os.O_WRONLY))
    fd = os.open(d, os.O_RDONLY | os.O_DIRECTORY)
    saved = tmp_path / "saved"
    saved.mkdir()
    (saved / "keep").write_text("1")
    t = time.time()
    assert server.data_save(fd, saved) is False
    os.close(fd)
    assert time.time() - t < 1.0
    assert os.listdir(saved) == ["keep"]


def test_rejected_seed_is_reported_so_the_old_data_is_not_replaced(tmp_path):
    saved = tmp_path / "saved"
    saved.mkdir()
    for i in range(server.SAVE_MAX_FILES + 5):
        (saved / ("f%d" % i)).write_text("")
    assert server.data_seed(saved, tmp_path / "seed") is False
    assert os.listdir(tmp_path / "seed") == []
    assert len(os.listdir(saved)) == server.SAVE_MAX_FILES + 5       # untouched
    (tmp_path / "ok").mkdir()
    assert server.data_seed(tmp_path / "ok", tmp_path / "seed2") is True
    assert server.data_seed(tmp_path / "missing", tmp_path / "seed3") is True


@needs_bwrap
def test_the_app_cannot_write_into_run_sim(sims):
    s = sims("dd if=/dev/zero of=/run/sim/big bs=1M count=50 2>/dev/null; echo $? > /data/sd/res; "
             "ls /run/sim > /data/sd/ls; touch /data/sd/done")
    s.wait_done()
    assert s.res().strip() != "0"
    assert "big" not in s.res("ls")
    assert os.listdir(s.dir / "run") == []


@needs_bwrap
def test_the_sim_socket_still_connects_through_the_read_only_run_dir(sims, tmp_path):
    import socket
    s = sims("sleep 0", name="x")      # only to get the dir layout; the socket is made below
    s.close()
    run = tmp_path / "run2"
    run.mkdir()
    srv = socket.socket(socket.AF_UNIX)
    srv.bind(str(run / "sim.sock"))
    os.chmod(run / "sim.sock", 0o777)
    srv.listen(1)
    elf = tmp_path / "c.sh"
    elf.write_text("#!/bin/sh\nexec python3 -c \"import socket;c=socket.socket(socket.AF_UNIX);c.connect('/run/sim/sim.sock')\"\n")
    elf.chmod(0o755)
    (tmp_path / "seed9").mkdir()
    cmd = server.sim_cmd(elf, run, tmp_path / "seed9")
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    srv.settimeout(10)
    conn, _ = srv.accept()
    p.wait()
