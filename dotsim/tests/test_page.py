"""The public page: kode-sim's web/index.html with dotsim/web/ext.js, in a real browser.

Starts the server on port 8899 against temp dirs, with apps/slang (Snake) built
for the host-sim (Task 2's cached build under /tmp when there is one), and
drives the page with Playwright's Chromium. Skipped where ESP-IDF or bwrap is
missing (both live in the sim CT).
"""
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path

import pytest

DOTSIM = Path(__file__).resolve().parents[1]
IDF = Path(os.environ.get("IDF_PATH", "/opt/esp/esp-idf"))
CACHED = Path(os.environ.get("DOTSIM_TEST_BUILD", "/tmp/dotsim-test-build")) / "slang" / "kode_sim_app.elf"
PORT = 8899
URL = f"http://127.0.0.1:{PORT}/"

pytestmark = pytest.mark.skipif(not (IDF.exists() or CACHED.exists()) or not shutil.which("bwrap"),
                                reason="needs the sim CT (ESP-IDF or a cached build, and bwrap)")


def snake_elf():
    # assembled either way: the editor reads apps/slang/main (kk.h comes from assemble.py)
    subprocess.run(["python3", "assemble.py", "slang"], cwd=DOTSIM / "apps", check=True)
    if CACHED.exists():
        return CACHED
    r = subprocess.run([str(DOTSIM / "hostsim" / "build.sh"), str(DOTSIM / "apps" / "slang"), str(CACHED.parent)],
                       capture_output=True, text=True, timeout=1800)
    assert r.returncode == 0 and CACHED.exists(), (r.stdout + r.stderr)[-3000:]
    return CACHED


@pytest.fixture(scope="module")
def server():
    elf = snake_elf()
    tmp = Path(tempfile.mkdtemp(prefix="dotsim-page-"))
    builds = tmp / "builds"
    (builds / "slang").mkdir(parents=True)
    shutil.copy(elf, builds / "slang" / "kode_sim_app.elf")
    (builds / "slang" / "kode-sim.json").write_text(json.dumps({"name": "Snake"}))
    env = dict(os.environ, KODE_PORT=str(PORT), KODE_BUILDS=str(builds), KODE_DATA=str(tmp / "data"),
               KODE_WORK=str(tmp / "work"), KODE_SIM=str(DOTSIM / "hostsim"),
               KODE_LOCAL_APPS=str(DOTSIM / "apps"))
    proc = subprocess.Popen([sys.executable, str(DOTSIM / "server" / "server.py")], env=env,
                            cwd=DOTSIM / "server", stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    end = time.time() + 20
    while True:
        try:
            with socket.create_connection(("127.0.0.1", PORT), 0.5):
                break
        except OSError:
            if proc.poll() is not None or time.time() > end:
                proc.kill()
                raise AssertionError("server did not start: " + proc.stderr.read().decode()[-2000:])
            time.sleep(0.2)
    yield URL
    proc.kill()
    proc.wait()
    shutil.rmtree(tmp, ignore_errors=True)


@pytest.fixture(scope="module")
def browser():
    from playwright.sync_api import sync_playwright
    with sync_playwright() as p:
        b = p.chromium.launch(args=["--autoplay-policy=no-user-gesture-required"])
        yield b
        b.close()


@pytest.fixture()
def page(server, browser):
    ctx = browser.new_context(viewport={"width": 1400, "height": 1000})
    pg = ctx.new_page()
    pg.errors = []
    pg.on("pageerror", lambda e: pg.errors.append("pageerror: %s" % e))
    pg.on("console", lambda m: m.type == "error" and pg.errors.append("console: %s" % m.text))
    pg.goto(server)
    pg.wait_for_function("window.DotSim && DotSim.ready", timeout=15000)
    yield pg
    ctx.close()


def lit_pixels(pg):
    """How many of a sample of the glass's pixels are not black."""
    return pg.evaluate("""() => {
        const d = document.getElementById('screen').getContext('2d').getImageData(0, 0, 410, 502).data;
        let n = 0;
        for (let i = 0; i < d.length; i += 4 * 97) if (d[i] + d[i + 1] + d[i + 2] > 30) n++;
        return n;
    }""")


def press(pg, name):
    el = pg.locator(f'[data-btn="{name}"]')
    el.dispatch_event("pointerdown", {"pointerId": 1, "isPrimary": True})
    pg.wait_for_timeout(120)
    el.dispatch_event("pointerup", {"pointerId": 1, "isPrimary": True})


def test_loads_clean(page):
    page.wait_for_timeout(1500)
    assert page.evaluate("DotSim.mode") in ("beginner", "advanced")
    assert page.evaluate("DotSim.engine") == "fast"
    assert "Unofficial. Not made, approved or supported by Kode." in page.inner_text("body")
    assert page.locator('a[href="https://github.com/Rein033/dot-sim"]').count() >= 1
    assert page.locator('a[href="/docs"]').count() >= 1
    # kode-sim's Spanish is gone from what shows
    assert "Ajustes" not in page.inner_text("#settings")
    assert page.errors == []


def test_home_then_grid(page):
    page.wait_for_function("DotSim.view === 'face'")
    page.wait_for_timeout(300)
    assert lit_pixels(page) > 20, "Dash's face is not on the glass"
    press(page, "top")
    page.wait_for_function("DotSim.view === 'grid'", timeout=3000)
    page.wait_for_function("DotSim.apps.some((a) => a.id === 'slang')", timeout=5000)
    assert page.errors == []


def test_snake_fps(page):
    page.evaluate("DotSim.start('slang')")
    page.wait_for_function("DotSim.running", timeout=20000)
    page.wait_for_function("DotSim.stats.frames > 0", timeout=10000)
    page.wait_for_timeout(1000)
    n0 = page.evaluate("DotSim.stats.frames")
    page.wait_for_timeout(3000)
    fps = (page.evaluate("DotSim.stats.frames") - n0) / 3
    assert fps > 20, "only %.1f fps" % fps
    assert lit_pixels(page) > 20
    page.evaluate("DotSim.home()")
    page.wait_for_function("DotSim.view !== 'app'", timeout=5000)
    assert page.errors == []


def test_battery_slider(page):
    page.evaluate("DotSim.setMode('advanced')")
    page.click("#settings")
    page.evaluate("DotSim.start('slang')")
    page.wait_for_function("DotSim.running", timeout=20000)
    page.evaluate("""() => { const r = document.getElementById('pct'); r.value = 37;
                             r.dispatchEvent(new Event('input', { bubbles: true })); }""")
    got = ""
    for _ in range(20):
        page.fill("#cmd", "? bat")
        page.press("#cmd", "Enter")
        page.wait_for_timeout(500)
        got = page.inner_text("#log")
        if "bat 37 1" in got:
            break
    assert "bat 37 1" in got, got[-800:]
    page.evaluate("DotSim.home()")
    assert page.errors == []


def test_public_paths(server):
    """Every file the page needs, at the paths the proxy lets through."""
    for path in ("", "sim.js", "audio-worklet.js", "kode-dot.png", "kode-dot.json", "dotsim/ext.js",
                 "dotsim/theme/theme.js", "dotsim/theme/theme.css", "dotsim/theme/dash.js", "docs"):
        with urllib.request.urlopen(URL + path) as r:
            assert r.status == 200, path
