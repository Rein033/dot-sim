"""The microSD's folder on the server: names are checked here, files are the session's own."""
import base64
import os

import pytest

import server


def b64(data):
    return base64.b64encode(data).decode()


@pytest.mark.parametrize("name", ["", ".", "..", "../x", "a/b", "/etc/passwd", "a\\b", "x..y", "a" * 65,
                                  "bad\nname", "nul\x00", None, 5])
def test_sd_name_traversal_refused(tmp_path, name):
    d = tmp_path / "sd"
    d.mkdir()
    assert server.sd_put(d, name, b64(b"hi")) is not None
    assert server.sd_read(d, name)[0] is None
    assert os.listdir(d) == []
    assert os.listdir(tmp_path) == ["sd"]          # nothing written next to the card either


def test_sd_put_ls_get(tmp_path):
    d = tmp_path / "sd"
    assert server.sd_put(d, "hello.txt", b64(b"hello")) is None
    assert server.sd_files(d) == [{"name": "hello.txt", "size": 5}]
    assert server.sd_read(d, "hello.txt") == (b"hello", None)
    assert server.sd_put(d, "a" * 64, b64(b"x")) is None   # 64 is the longest


def test_sd_limits(tmp_path):
    d = tmp_path / "sd"
    assert server.sd_put(d, "big", b64(b"x" * (700 * 1024 + 1))) is not None
    assert server.sd_put(d, "ok", b64(b"x" * (700 * 1024))) is None
    for i in range(22):                              # 23 x 700 KB > 16 MB
        server.sd_put(d, "f%d" % i, b64(b"x" * (700 * 1024)))
    assert sum(f["size"] for f in server.sd_files(d)) <= 16 * 1024 * 1024
    assert server.sd_put(d, "ok", b64(b"y" * 10)) is None   # replacing a file frees its size


def test_sd_symlink_from_the_app_is_not_followed(tmp_path):
    d = tmp_path / "sd"
    d.mkdir()
    secret = tmp_path / "secret"
    secret.write_text("no")
    (d / "link").symlink_to(secret)
    assert server.sd_files(d) == []
    assert server.sd_read(d, "link")[0] is None
    assert server.sd_put(d, "link", b64(b"x")) is not None
    assert secret.read_text() == "no"
