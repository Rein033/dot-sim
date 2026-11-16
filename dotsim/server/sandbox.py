"""The sandboxes: the command lines of a sim and of a build, and the sim's saved /data.

A sim runs under prlimit (memory, CPU time) in bubblewrap: no network, read-only system, the app's ELF and
its run folder (the bridge's socket) read-only, and three size-capped tmpfs mounts:

  /tmp      32 MB
  /data     16 MB, starts as a copy of the app's saved data
  /data/sd  16 MB, the microSD (sdcard.py)

/data starts as a copy of the app's saved data (NVS, high scores, storage/) and is copied back when the
session ends, only if it is small. bwrap 0.8 cannot cap inodes, so the copy-back caps the file count.
Saved data is never trusted: both copies go through tree_copy, which uses lstat/O_NOFOLLOW directory fds
and copies only regular files and folders. Copy-back reads the sandbox's /data through a directory fd opened
once at start (via /proc/<pid>/root, like the card); the fd keeps the tmpfs readable after the app has died.
"""
import logging
import os
import shutil
import stat
import tempfile
import threading
from pathlib import Path

from config import SDK, SIM, WORK
from sdcard import SD_MOUNT_ARGS

log = logging.getLogger("kode")

SIM_PRLIMIT = ["prlimit", "--as=1500000000", "--cpu=3600", "--"]
TMP_MOUNT_ARGS = ["--size", str(32 * 1024 * 1024), "--tmpfs", "/tmp"]
DATA_MOUNT_ARGS = ["--size", str(16 * 1024 * 1024), "--tmpfs", "/data"]
SAVE_MAX_BYTES = 1024 * 1024
SAVE_MAX_FILES = 200
SAVE_MAX_DEPTH = 8


def sim_cmd(elf, run_dir, seed):
    """The sandbox command line of a sim. /data starts as a copy of @p seed, which is mounted read-only at /seed."""
    return [
        "bwrap", "--ro-bind", "/usr", "/usr", "--ro-bind", "/lib", "/lib",
        "--ro-bind-try", "/lib64", "/lib64", "--ro-bind-try", "/lib32", "/lib32",
        "--ro-bind-try", "/bin", "/bin",
        "--ro-bind", "/etc/ld.so.cache", "/etc/ld.so.cache",
        "--ro-bind", str(elf), "/app.elf",
        "--ro-bind", str(run_dir), "/run/sim",    # the app may connect to the socket, not write
        "--ro-bind", str(seed), "/seed",
        *DATA_MOUNT_ARGS,
        *SD_MOUNT_ARGS,
        "--dev", "/dev", "--proc", "/proc", *TMP_MOUNT_ARGS,
        "--unshare-all", "--die-with-parent", "--new-session",
        "--setenv", "KODE_SIM_SOCK", "/run/sim/sim.sock",
        "--setenv", "KODE_SIM_DATA", "/data",
        "--chdir", "/data",
        "/bin/sh", "-c", "cp -R /seed/. /data/ 2>/dev/null; exec /app.elf",
    ]


def sd_child_pid(pid):
    """The first process inside the sandbox that @p pid (bwrap) started, or None."""
    try:
        kids = Path(f"/proc/{pid}/task/{pid}/children").read_text().split()
        return int(kids[0]) if kids else None
    except (OSError, ValueError):
        return None


class TreeTooBig(Exception):
    pass


def _tree_copy(src_fd, dest, state, depth, skip=()):
    if depth > SAVE_MAX_DEPTH:
        raise TreeTooBig("too deep")
    dest.mkdir(exist_ok=True)
    seen = 0
    for entry in os.scandir(src_fd):
        name = entry.name
        seen += 1
        if seen > SAVE_MAX_FILES + len(skip):      # stop reading a huge folder at the cap
            raise TreeTooBig("more than %d files" % SAVE_MAX_FILES)
        if name in skip:
            continue
        try:
            st = entry.stat(follow_symlinks=False)
        except OSError:
            continue
        if stat.S_ISDIR(st.st_mode):
            state[0] += 1
            if state[0] > SAVE_MAX_FILES:
                raise TreeTooBig("more than %d files" % SAVE_MAX_FILES)
            try:
                sub = os.open(name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=src_fd)
            except OSError:
                continue
            try:
                _tree_copy(sub, dest / name, state, depth + 1)
            finally:
                os.close(sub)
        elif stat.S_ISREG(st.st_mode):
            state[0] += 1
            if state[0] > SAVE_MAX_FILES:
                raise TreeTooBig("more than %d files" % SAVE_MAX_FILES)
            try:
                fd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=src_fd)
            except OSError:
                continue
            try:
                if not stat.S_ISREG(os.fstat(fd).st_mode):
                    continue
                with open(dest / name, "wb") as f:
                    while True:
                        data = os.read(fd, 65536)
                        if not data:
                            break
                        state[1] += len(data)
                        if state[1] > SAVE_MAX_BYTES:
                            raise TreeTooBig("more than %d bytes" % SAVE_MAX_BYTES)
                        f.write(data)
            finally:
                os.close(fd)


def tree_copy(src_fd, dest, skip=()):
    """Copy the regular files and folders below the directory fd to @p dest (<=1 MB, <=200 files); raises TreeTooBig."""
    _tree_copy(src_fd, Path(dest), [0, 0], 0, skip)


def data_seed(saved, dest):
    """Stage the app's saved data into @p dest for the new sandbox. Returns False (empty data, logged) if
    the saved data is unusable; the caller must then not overwrite it at the end."""
    dest = Path(dest)
    dest.mkdir(exist_ok=True)
    try:
        fd = os.open(saved, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    except OSError:
        return True
    try:
        tree_copy(fd, dest, skip=("sd",))
    except (TreeTooBig, OSError) as e:
        log.warning("data: saved data of %s not loaded, it will not be replaced either: %s", saved, e)
        shutil.rmtree(dest, ignore_errors=True)
        dest.mkdir()
        return False
    finally:
        os.close(fd)
    return True


def data_open(pid):
    """A directory fd on the sandbox's /data, or None."""
    try:
        return os.open(f"/proc/{pid}/root/data", os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    except OSError:
        return None


_save_locks = {}
_save_locks_guard = threading.Lock()


def data_save(fd, saved):
    """Copy the sandbox's /data (not the card) over the saved data; keep the old one and log why if it is too big.
    One save per app at a time (the last to get the lock wins); each uses its own staging folder."""
    saved = Path(saved)
    with _save_locks_guard:
        lock = _save_locks.setdefault(saved.name, threading.Lock())
    with lock:
        new = Path(tempfile.mkdtemp(dir=saved.parent, prefix=saved.name + ".new-"))
        try:
            tree_copy(fd, new, skip=("sd",))
        except (TreeTooBig, OSError) as e:
            log.warning("data: not saved for %s, the old data stays: %s", saved.name, e)
            shutil.rmtree(new, ignore_errors=True)
            return False
        old = Path(tempfile.mkdtemp(dir=saved.parent, prefix=saved.name + ".old-"))
        try:
            if saved.exists():
                saved.rename(old / "x")
            new.rename(saved)
        except OSError as e:
            log.warning("data: not saved for %s: %s", saved.name, e)
            shutil.rmtree(new, ignore_errors=True)
            return False
        finally:
            shutil.rmtree(old, ignore_errors=True)
        return True


# A build runs the visitor's code through ESP-IDF: read-only system and SDK, writable only WORK, no network.
BUILD_PRLIMIT = ["prlimit", "--as=4000000000", "--cpu=300", "--"]


def build_cmd(app_dir, out):
    """The sandbox command line of one build (hostsim/build.sh <app_dir> <out>), without its prlimit."""
    return [
        "bwrap", "--ro-bind", "/usr", "/usr",
        "--symlink", "usr/lib", "/lib", "--symlink", "usr/lib32", "/lib32",
        "--symlink", "usr/lib64", "/lib64", "--symlink", "usr/bin", "/bin",
        "--symlink", "usr/sbin", "/sbin", "--ro-bind", "/etc", "/etc",
        "--ro-bind", "/opt/esp", "/opt/esp",
        "--ro-bind", str(SIM), str(SIM), "--ro-bind", str(SDK), str(SDK),
        "--bind", str(WORK), str(WORK),
        "--dev", "/dev", "--proc", "/proc", "--tmpfs", "/tmp",
        "--unshare-all", "--die-with-parent", "--new-session",
        "--setenv", "HOME", str(WORK / "home"),
        str(SIM / "build.sh"), str(app_dir), str(out),
    ]
