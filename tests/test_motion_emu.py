"""The IMU and the magnetometer under kode-sdk's own drivers: kode-sdk's
`examples/imu` reads what the page poses, through kdrv_qmi8658 and
kdrv_qmc5883p, on esp-emu's I2C."""

import asyncio
import re
import struct

import pytest

import kd
from emu_helpers import boot, console_line, firmware, no_panic

pytestmark = pytest.mark.emu


@pytest.fixture(scope="module")
def imu(exe, kode_sdk):
    return firmware(exe, kode_sdk / "examples" / "imu", sdk=kode_sdk)


def motion(accel_mg, gyro_mdps, mag_mg, temp_dc=250):
    return struct.pack("<10i", *accel_mg, *gyro_mdps, *mag_mg, temp_dc)


def close(got, want, tol):
    return all(abs(g - w) <= tol for g, w in zip(got, want))


async def test_the_sdk_reads_the_pose_the_page_sets(exe, imu, tmp_path):
    """kode-sdk's drivers, unchanged, read in the board's axes what the page
    sets: acceleration and rotation from the QMI8658A, the field from the
    QMC5883P, at the full scale and rate the drivers chose."""
    k = await boot(exe, tmp_path, imu)
    try:
        await console_line(k, r"imu  a=\(", 60)                      # streaming, at the desk's rest
        since = len(k.console)
        want_a, want_w, want_m = (100, -200, 980), (1500, 0, -750), (300, 0, -400)
        await k.send(kd.MOTION, motion(want_a, want_w, want_m))
        a_ok = m_ok = None
        loop = asyncio.get_running_loop()
        end = loop.time() + 20
        while loop.time() < end and not (a_ok and m_ok):
            for line in k.console[since:]:
                m = re.search(r"imu  a=\((-?\d+), (-?\d+), (-?\d+)\) mg  w=\((-?\d+), (-?\d+), (-?\d+)\)", line)
                if m and close([int(x) for x in m.groups()[:3]], want_a, 8) \
                        and close([int(x) for x in m.groups()[3:]], want_w, 80):
                    a_ok = line
                m = re.search(r"mag  m=\((-?\d+), (-?\d+), (-?\d+)\)", line)
                if m and close([int(x) for x in m.groups()], want_m, 10):
                    m_ok = line
            since = len(k.console)
            await asyncio.sleep(0.2)
        said = "\n".join(l for l in k.console if " a=(" in l or " m=(" in l)[-1500:]
        assert a_ok, "the IMU never read the pose:\n" + said
        assert m_ok, "the magnetometer never read the field:\n" + said
        no_panic(k)
    finally:
        await k.stop()
