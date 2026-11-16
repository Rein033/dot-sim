import struct

import pytest

import kd
from emu_helpers import boot, firmware, frame_where

pytestmark = pytest.mark.emu


async def test_smoke_hello(exe, kode_sdk, tmp_path):
    fw = firmware(exe, kode_sdk / "examples" / "buttons_led", sdk=kode_sdk)
    k = await boot(exe, tmp_path, fw)
    try:
        _, p = await frame_where(k, lambda t, p: t == kd.HELLO, 60)
        assert struct.unpack("<HHH", p) == (1, 410, 502)
    finally:
        await k.stop()
