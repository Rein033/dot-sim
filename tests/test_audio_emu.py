import asyncio
import math
import re
import struct

import pytest

import kd
from emu_helpers import boot, console_line, firmware, frame_where, no_panic
from test_io_emu import click

pytestmark = pytest.mark.emu


@pytest.fixture(scope="module")
def audio(exe, kode_sdk):
    return firmware(exe, kode_sdk / "examples" / "audio", sdk=kode_sdk)


async def test_note_reaches_the_mac(exe, audio, tmp_path):
    k = await boot(exe, tmp_path, audio)
    try:
        await console_line(k, r"audio up; the top button plays a note", 60)
        await frame_where(k, lambda t, p: t == kd.HELLO, 5)
        await click(k, kd.BTN_TOP)                     # one 250 ms note
        frames, sq, n, rate, ch = 0, 0.0, 0, 0, 1
        mic = False
        loop = asyncio.get_running_loop()
        end = loop.time() + 6
        while loop.time() < end:
            try:
                t, p = await frame_where(k, lambda t, p: t in (kd.PCM_OUT, kd.MIC_START), end - loop.time())
            except (asyncio.TimeoutError, TimeoutError):
                break
            if t == kd.MIC_START:
                mic = True
                continue
            rate, ch = struct.unpack_from("<IB", p)
            s = memoryview(p[5:]).cast("h")
            frames += len(s) // ch
            sq += sum(v * v for v in s)
            n += len(s)
        assert rate == 48000
        assert n and math.sqrt(sq / n) > 100, "silence"
        assert not mic and not any(t == kd.MIC_START for t, _ in k.backlog), "a note asked for the mic"
        ms = frames * 1000 / rate
        assert 200 <= ms <= 1000, f"{ms:.0f} ms of sound for a 250 ms note"
        no_panic(k)
    finally:
        await k.stop()


async def test_mic_hears_the_mac(exe, audio, tmp_path):
    k = await boot(exe, tmp_path, audio)
    try:
        await console_line(k, r"kaos\.audio up", 60)          # the ES8311 found and parked
        await asyncio.sleep(1)

        async def feed():
            t = 0
            while True:
                _, p = await frame_where(k, lambda t, p: t == kd.MIC_START, 30)
                rate = struct.unpack_from("<I", p)[0]
                while True:
                    chunk = struct.pack("<960h", *(int(8000 * math.sin(2 * math.pi * 1000 * (t + i) / rate))
                                                    for i in range(960)))
                    t += 960
                    await k.send(kd.PCM_IN, chunk)
                    await asyncio.sleep(960 / rate)

        feeder = asyncio.create_task(feed())
        await k.send_json({"mic": True})                # the page's switch
        await k.console_in("test component=audio")
        line = await console_line(k, r"test audio: floor rms=", 60)
        feeder.cancel()
        # rms is dBFS x10: an 8000-peak sine is -15 dBFS, silence far below.
        rms = int(re.search(r"rms=(-?\d+)", line).group(1))
        assert rms > -250, line
        no_panic(k)
    finally:
        await k.stop()


async def test_the_audio_self_test_passes_on_a_quiet_desk(exe, audio, tmp_path):
    """`test component=audio` on a healthy board in a quiet room passes: the
    ES8311's ADC has a floor of its own (never constant zeros - that is a dead
    I2S, and the test says so) and the microphone hears the board's own
    speaker play the test's tones. No microphone from the Mac at all."""
    k = await boot(exe, tmp_path, audio)
    try:
        await console_line(k, r"kaos\.audio up", 60)          # the ES8311 found and parked
        await asyncio.sleep(1)
        await k.console_in("test component=audio")
        line = await console_line(k, r"^>\|test=audio result=", 90)
        assert "result=pass" in line, "\n".join(l for l in k.console if "audio" in l or l.startswith(">|step"))[-3000:]
        no_panic(k)
    finally:
        await k.stop()


async def test_the_speaker_crosses_the_wire_mono(exe, audio, tmp_path):
    """The ES8311 has one DAC, playing the left slot, and the board one
    speaker: the page gets that one channel, not the two identical slots the
    I2S frames carry - half the bytes for every sound."""
    k = await boot(exe, tmp_path, audio)
    try:
        await console_line(k, r"audio up; the top button plays a note", 60)
        await click(k, kd.BTN_TOP)
        _, p = await frame_where(k, lambda t, p: t == kd.PCM_OUT, 10)
        rate, ch = struct.unpack_from("<IB", p)
        assert (rate, ch) == (48000, 1), (rate, ch)
        no_panic(k)
    finally:
        await k.stop()
