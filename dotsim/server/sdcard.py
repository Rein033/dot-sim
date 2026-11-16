"""The microSD: names and sizes are checked here, never trusted from the page.

The card is a size-limited tmpfs inside the sandbox (bwrap --size, SD_MOUNT_ARGS): whatever the app
writes there stops at 16 MB. The server reaches it through /proc/<sandbox pid>/root, opened ONCE as a
directory fd (sd_open); every later call works relative to that fd, so nothing the app does to the
files (links, folders, renames) can lead the server outside the card.
"""
import base64
import os
import stat
import uuid

SD_FILE_MAX = 700 * 1024        # one upload
SD_TOTAL_MAX = 16 * 1024 * 1024  # everything on the card, per session (also the tmpfs size)
SD_NAME_MAX = 64
SD_LIST_MAX = 500
SD_MOUNT_ARGS = ["--size", str(SD_TOTAL_MAX), "--tmpfs", "/data/sd"]
SD_TMP = ".put-"


def sd_name_ok(name):
    """A plain file name: no path, no '..', not empty, at most 64 characters and 255 bytes of UTF-8."""
    if not (isinstance(name, str) and 0 < len(name) <= SD_NAME_MAX):
        return False
    try:
        if len(name.encode("utf-8")) > 255:
            return False
    except UnicodeError:       # a lone surrogate from the page's JSON
        return False
    return ("/" not in name and "\\" not in name and name != "." and ".." not in name
            and not any(ord(c) < 32 or ord(c) == 127 for c in name))


def sd_open(pid):
    """A directory fd on the card of the sandbox whose init (or app) is @p pid, or None."""
    try:
        return os.open(f"/proc/{pid}/root/data/sd", os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    except OSError:
        return None


def sd_files(dfd):
    """[{name, size}] of the regular files on the card, at most SD_LIST_MAX (links and folders are not files
    here: the app that runs on the card can make them)."""
    out = []
    try:
        with os.scandir(dfd) as it:
            for e in it:
                if e.name.startswith(SD_TMP):
                    continue
                st = e.stat(follow_symlinks=False)
                if stat.S_ISREG(st.st_mode):
                    out.append({"name": e.name, "size": st.st_size})
    except OSError:
        pass
    out.sort(key=lambda f: f["name"])
    return out[:SD_LIST_MAX]


def sd_put(dfd, name, b64):
    """Write one uploaded file onto the card; returns an error text, or None when it is stored."""
    try:
        return _sd_put(dfd, name, b64)
    except (OSError, UnicodeError) as e:
        return "the card refused it (%s)" % (getattr(e, "strerror", None) or "bad name")


def _sd_put(dfd, name, b64):
    if not sd_name_ok(name):
        return "that file name is not allowed (no / or .., at most 64 characters)"
    if not isinstance(b64, str) or len(b64) > SD_FILE_MAX * 4 // 3 + 8:
        return "the file is bigger than 700 KB"
    try:
        data = base64.b64decode(b64, validate=True)
    except ValueError:
        return "the file did not arrive whole"
    if len(data) > SD_FILE_MAX:
        return "the file is bigger than 700 KB"
    files = sd_files(dfd)
    old = next((f["size"] for f in files if f["name"] == name), 0)
    if sum(f["size"] for f in files) - old + len(data) > SD_TOTAL_MAX:
        return "the card would hold more than 16 MB"
    try:
        st = os.stat(name, dir_fd=dfd, follow_symlinks=False)
        if not stat.S_ISREG(st.st_mode):
            return "that name is taken by something that is not a file"
    except FileNotFoundError:
        pass
    tmp = SD_TMP + uuid.uuid4().hex
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o666, dir_fd=dfd)
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
        os.rename(tmp, name, src_dir_fd=dfd, dst_dir_fd=dfd)
    except BaseException:
        try:
            os.unlink(tmp, dir_fd=dfd)
        except OSError:
            pass
        raise
    return None


def sd_read(dfd, name):
    """(bytes, None) of one file on the card, or (None, error text)."""
    if not sd_name_ok(name):
        return None, "that file name is not allowed"
    try:
        fd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=dfd)
        with os.fdopen(fd, "rb") as f:
            st = os.fstat(f.fileno())
            if not stat.S_ISREG(st.st_mode):
                return None, "no such file"
            if st.st_size > SD_FILE_MAX:
                return None, "the file is bigger than 700 KB"
            return f.read(SD_FILE_MAX + 1), None
    except (OSError, UnicodeError):
        return None, "no such file"
