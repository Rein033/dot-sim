"""The microSD's card on the server: names are checked here, the card is a size-limited folder of the sandbox."""
import base64
import os
import shutil
import subprocess
import time

import pytest

import server


def b64(data):
    return base64.b64encode(data).decode()


@pytest.fixture()
def card(tmp_path):
    d = tmp_path / "sd"
    d.mkdir()
    fd = os.open(d, os.O_RDONLY | os.O_DIRECTORY)
    yield fd, d
    os.close(fd)


@pytest.mark.parametrize("name", ["", ".", "..", "../x", "a/b", "/etc/passwd", "a\\b", "x..y", "a" * 65,
                                  "bad\nname", "nul\x00", None, 5, "\ud800", "a\ud800b", "\U0001F600" * 64])
def test_sd_name_traversal_refused(tmp_path, card, name):
    fd, d = card
    assert server.sd_put(fd, name, b64(b"hi")) is not None      # an error text, never an exception
    assert server.sd_read(fd, name)[0] is None
    assert os.listdir(d) == []
    assert os.listdir(tmp_path) == ["sd"]          # nothing written next to the card either


def test_sd_name_bytes(card):
    fd, _ = card
    assert not server.sd_name_ok("\U0001F600" * 64)    # 256 bytes of UTF-8
    assert server.sd_put(fd, "\U0001F600" * 63, b64(b"x")) is None   # 252 bytes: fits


def test_sd_put_ls_get(card):
    fd, _ = card
    assert server.sd_put(fd, "hello.txt", b64(b"hello")) is None
    assert server.sd_files(fd) == [{"name": "hello.txt", "size": 5}]
    assert server.sd_read(fd, "hello.txt") == (b"hello", None)
    assert server.sd_put(fd, "a" * 64, b64(b"x")) is None   # 64 is the longest


def test_sd_limits(card):
    fd, _ = card
    assert server.sd_put(fd, "big", b64(b"x" * (700 * 1024 + 1))) is not None
    assert server.sd_put(fd, "ok", b64(b"x" * (700 * 1024))) is None
    for i in range(22):                              # 23 x 700 KB > 16 MB
        server.sd_put(fd, "f%d" % i, b64(b"x" * (700 * 1024)))
    assert sum(f["size"] for f in server.sd_files(fd)) <= 16 * 1024 * 1024
    assert server.sd_put(fd, "ok", b64(b"y" * 10)) is None   # replacing a file frees its size


def test_sd_listing_is_capped(card):
    fd, d = card
    for i in range(600):
        (d / ("f%03d" % i)).write_bytes(b"")
    assert len(server.sd_files(fd)) == server.SD_LIST_MAX


def test_sd_put_error_is_a_message(card, monkeypatch):
    fd, _ = card
    def boom(*a, **k):
        raise OSError(28, "No space left on device")
    monkeypatch.setattr(server.os, "rename", boom)
    assert "No space" in server.sd_put(fd, "x", b64(b"x"))
    assert [f for f in server.sd_files(fd)] == []            # the temp file is gone too


def test_sd_symlink_from_the_app_is_not_followed(tmp_path, card):
    fd, d = card
    secret = tmp_path / "secret"
    secret.write_text("no")
    (d / "link").symlink_to(secret)
    (d / "folder").mkdir()
    assert server.sd_files(fd) == []
    assert server.sd_read(fd, "link")[0] is None
    assert server.sd_put(fd, "link", b64(b"x")) is not None
    assert server.sd_put(fd, "folder", b64(b"x")) is not None
    assert secret.read_text() == "no"


def test_sd_folder_swapped_for_a_symlink_stays_on_the_card(tmp_path, card):
    """The directory fd is the card: renaming its folder and putting a link to the host in its place changes nothing."""
    fd, d = card
    assert server.sd_put(fd, "mine.txt", b64(b"mine")) is None
    host = tmp_path / "host"
    host.mkdir()
    (host / "passwd").write_text("root:x")
    d.rename(tmp_path / "moved")
    d.symlink_to(host)
    assert server.sd_files(fd) == [{"name": "mine.txt", "size": 4}]
    assert server.sd_read(fd, "passwd")[0] is None
    assert server.sd_read(fd, "mine.txt") == (b"mine", None)
    assert server.sd_put(fd, "new.txt", b64(b"n")) is None
    assert sorted(os.listdir(host)) == ["passwd"]            # nothing landed on the host side
    assert (tmp_path / "moved" / "new.txt").exists()


needs_bwrap = pytest.mark.skipif(not shutil.which("bwrap"), reason="needs bwrap")


@pytest.fixture()
def sandbox():
    """A sandbox like the sim's, with the card mounted as the server does, and a shell standing in for the app."""
    procs = []

    def run(script):
        cmd = ["bwrap", "--ro-bind", "/usr", "/usr", "--ro-bind", "/lib", "/lib",
               "--ro-bind-try", "/lib64", "/lib64", "--ro-bind-try", "/bin", "/bin", *server.DATA_MOUNT_ARGS,
               *server.SD_MOUNT_ARGS, "--dev", "/dev", "--proc", "/proc", "--unshare-all", "--die-with-parent",
               "--new-session", "sh", "-c", script]
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        procs.append(p)
        return p

    yield run
    for p in procs:
        p.kill()
        p.wait()


def card_of(p):
    end = time.time() + 10
    while time.time() < end:
        kid = server.sd_child_pid(p.pid)
        fd = server.sd_open(kid) if kid else None
        if fd is not None:
            return fd
        time.sleep(0.1)
    raise AssertionError("no card of the sandbox")


@needs_bwrap
def test_app_writes_past_16mb_are_stopped(sandbox):
    p = sandbox("dd if=/dev/zero of=/data/sd/big bs=1M count=40 2>/dev/null; dd if=/dev/zero of=/data/sd/more bs=1M count=5 2>/dev/null; touch /data/sd/done; sleep 30")
    fd = card_of(p)
    try:
        end = time.time() + 10
        while time.time() < end and not any(f["name"] == "done" for f in server.sd_files(fd)):
            time.sleep(0.1)
        total = sum(f["size"] for f in server.sd_files(fd))
        assert 0 < total <= server.SD_TOTAL_MAX, total
        # the card is full, and the server says so in words
        assert server.sd_put(fd, "x.txt", b64(b"x" * 1000)) is not None
    finally:
        os.close(fd)


@needs_bwrap
def test_app_swaps_its_card_for_a_link_to_the_host(sandbox, tmp_path):
    """The app can rename and link whatever it likes on /data; the server's fd stays on the card."""
    p = sandbox("echo mine > /data/sd/mine.txt; ln -s /etc /data/sd/etc; mv /data/sd /data/sd2 2>/dev/null; "
                "ln -s /etc /data/sd 2>/dev/null; touch /data/ready; sleep 30")
    fd = card_of(p)
    try:
        time.sleep(1)
        names = [f["name"] for f in server.sd_files(fd)]
        assert "passwd" not in names and "etc" not in names
        assert server.sd_read(fd, "etc")[0] is None
        assert server.sd_read(fd, "hostname")[0] is None
    finally:
        os.close(fd)
