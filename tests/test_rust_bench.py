"""`kode-sim bench`, from outside: the binary and the fake esp-emu.

The fake's clock runs at $FAKE_CLOCK times the wall's and it paints
$FAKE_FPS times a second, so the speed and the frames bench reports are
known in advance.
"""

import json
import os
import subprocess


def bench(exe, tmp_path, *args, **env):
    (tmp_path / "m.bin").write_bytes(b"\xe9" + bytes(1000))
    return subprocess.run([str(exe), "bench", "--firmware", str(tmp_path / "m.bin"), "--elf", str(tmp_path / "a.elf"),
                           *args],
                          env={**os.environ, "KODE_SIM_CACHE": str(tmp_path / "cache"), **env},
                          capture_output=True, text=True, timeout=60)


def test_bench_measures_the_emulated_time_against_the_wall_s(exe, fake_emu, tmp_path):
    r = bench(exe, tmp_path, "--seconds", "2", "--json", FAKE_CLOCK="0.5", FAKE_FPS="20")
    assert r.returncode == 0, r.stderr
    j = json.loads(r.stdout)
    assert list(j) == ["firmware", "host_cpu", "host_cores", "esp_emu", "ratio", "emulated_s", "wall_s", "frames",
                       "fps", "cpu_s", "cpu_wall_s", "boot_s", "boot_emulated_s"]
    assert j["firmware"] == "m.bin" and j["esp_emu"] == "0.0.0-fake"
    assert j["host_cpu"] and j["host_cores"] >= 1
    assert 2 <= j["wall_s"] < 2.5, j
    assert 0.4 < j["ratio"] < 0.6, j
    assert abs(j["emulated_s"] - j["ratio"] * j["wall_s"]) < 0.01, j
    assert 10 < j["fps"] < 25 and j["frames"] > 20, j            # 20 a second, on a busy machine fewer
    assert j["cpu_wall_s"] >= j["wall_s"] and j["cpu_s"] > 0, j  # the fake's run, boot included
    assert j["boot_s"] > 0, j
    assert "measuring for 2 s" in r.stderr


def test_bench_says_it_in_words_without_json(exe, fake_emu, tmp_path):
    r = bench(exe, tmp_path, "--seconds", "1", FAKE_CLOCK="1")
    assert r.returncode == 0, r.stderr
    lines = r.stdout.splitlines()
    assert lines[0] == "kode-sim bench: m.bin on esp-emu 0.0.0-fake", r.stdout
    speed = next(l for l in lines if l.lstrip().startswith("speed"))
    assert "× real time" in speed and " s emulated in " in speed, r.stdout


def test_bench_of_a_firmware_that_crashes_fails(exe, fake_emu, tmp_path):
    r = bench(exe, tmp_path, "--seconds", "5", FAKE_CLOCK="1", FAKE_CRASH_AFTER="1")
    assert r.returncode == 1
    assert "crashed" in r.stderr, r.stderr
    assert r.stdout == ""


def test_bench_wants_a_project_or_a_firmware(exe, tmp_path):
    r = subprocess.run([str(exe), "bench"], capture_output=True, text=True, timeout=30)
    assert r.returncode == 2 and "<PROJECT>" in r.stderr, r.stderr
    r = subprocess.run([str(exe), "bench", "--seconds", "0", "x"], capture_output=True, text=True, timeout=30)
    assert r.returncode == 2 and "--seconds" in r.stderr, r.stderr
