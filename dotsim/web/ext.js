/* Dot Sim on kode-sim's page.
 *
 * web/index.html is kode-sim's own page, unchanged but for one line that loads
 * this file after sim.js. Everything Dot Sim adds happens here, at run time, so
 * upstream merges of web/ stay clean:
 *
 *   - the wire: sim.js speaks KD frames on <page>/ws; our server wants ?app=<id>
 *     on that socket and draws the screen as FB_JPEG (0x13), a type sim.js does
 *     not know. sim.js is a classic script, so its top-level names (ws, connect,
 *     onFrame, onJson, setState, ctx2d, mask, button, log, retryMs) are shared
 *     with this one: we wrap them instead of editing sim.js.
 *   - window.DotSim, for the other files and later tasks: mode, engine,
 *     start(appId), home(), apps, view, stats, and a tiny event bus.
 *   - the keys that are ours (Esc, Z/X, TOP+BOT held for 1 s), and input on the
 *     glass while no app runs (the home screen is ours, see home.js).
 *   - then it loads the theme and the rest: english.js (kode-sim's Spanish to
 *     English), shell.js (header, mode switch, footer), home.js (Dash and the
 *     app grid), editor.js (the code editor), parts.js (the board's parts: the microSD and what comes next).
 *
 * Not made, approved or supported by Kode.
 */
(function () {
  'use strict';
  const FB_JPEG = 0x13;
  const BASE = '/dotsim/';
  const byId = (id) => document.getElementById(id);
  const store = {
    get(k) { try { return localStorage.getItem(k); } catch (e) { return null; } },
    set(k, v) { try { localStorage.setItem(k, v); } catch (e) { /* not kept */ } },
  };
  const bus = {};

  const DS = window.DotSim = {
    ready: false,
    mode: store.get('dotsim.mode') === 'advanced' ? 'advanced' : 'beginner',
    engine: store.get('dotsim.engine') === 'exact' ? 'exact' : 'fast',   // only 'fast' runs today
    app: null,          // the app id on the socket, null on the home screen
    label: '',
    running: false,     // the server said "started"
    view: 'face',       // 'face' | 'grid' (home.js) | 'app'
    apps: [],           // from /api/status, in grid order
    user: '',
    version: '',
    stats: { frames: 0, fps: 0 },
    store,
    listen(name, fn) { (bus[name] = bus[name] || []).push(fn); },
    emit(name, ...args) { for (const fn of bus[name] || []) { try { fn(...args); } catch (e) { console.warn(e); } } },
    log(line) { window.log(line); },                          // sim.js's console
    setMode(m) {
      DS.mode = m === 'advanced' ? 'advanced' : 'beginner';
      store.set('dotsim.mode', DS.mode);
      DS.emit('mode', DS.mode);
    },
  };

  // ---- The wire ----------------------------------------------------------------

  // A socket we are done with: no more of its events reach sim.js. One still
  // connecting is left to finish and closed then (closing it now logs an error).
  function drop(s) {
    if (!s) return;
    s.onopen = s.onmessage = s.onclose = s.onerror = null;
    if (s.readyState === 1) s.close();
    else if (s.readyState === 0) s.onopen = () => s.close();
  }
  // sim.js connected at load, with no app: not ours to keep.
  drop(ws);
  ws = null;

  // sim.js's socket, with the app to run on it.
  const NativeWebSocket = window.WebSocket;
  // Only the newest one lives: any socket it supersedes is dropped, so no
  // stray session keeps a sim (and a server slot) running.
  const live = new Set();
  class AppSocket extends NativeWebSocket {
    constructor(url, protocols) {
      const u = new URL(url, location.href);
      u.searchParams.set('app', DS.app || '');
      super(u.href, protocols);
      for (const old of live) drop(old);
      live.clear();
      live.add(this);
      this.addEventListener('close', () => live.delete(this));
    }
  }
  window.WebSocket = AppSocket;

  const sim = { connect: window.connect, onFrame: window.onFrame, onJson: window.onJson, setState: window.setState };

  // Only with an app chosen (on the home screen there is nothing to connect to),
  // and never next to a socket that is open or opening: sim.js's onclose sets a
  // reconnect timer, and DS.start may have connected again before it fires.
  window.connect = () => {
    if (!DS.app || ending || (ws && ws.readyState <= 1)) return;
    sim.connect();
  };

  window.onFrame = (type, buf, off, len) => {
    if (type === FB_JPEG) jpeg(buf, off, len);
    else sim.onFrame(type, buf, off, len);
  };

  window.onJson = (j) => {
    if (j.t) ours(j);
    DS.emit('json', j);                  // the parts (parts.js) listen for their own keys
    sim.onJson(j);
  };

  // The server's own messages (server.py serve_ws, session.py Session), next to sim.js's.
  function ours(j) {
    if (j.t === 'started') {
      DS.running = true;
      window.setState('running');
      DS.emit('running', DS.app);
    } else if (j.t === 'exit') {
      DS.log('— app stopped: ' + j.why);
      homeSoon();
    } else if (j.t === 'ended') {
      if (j.err) DS.log(j.err);
      homeSoon();
    } else if (j.t === 'error') {
      DS.log('error: ' + j.s);
      DS.emit('error', j.s);
      DS.home();
    }
  }

  const STATE = { running: 'running', off: 'off', crashed: 'crashed', busy: 'waiting' };
  window.setState = (s, detail = '', local = false) => {
    sim.setState(s, detail, local);       // the power and reset buttons, the mic, a blank glass when off
    byId('state').textContent = DS.app ? (STATE[s] || s) : 'home';
    byId('power').textContent = s === 'running' ? 'Running' : 'Start';
    const b = byId('banner');
    b.replaceChildren();
    if (!DS.app || ending || s === 'running') { b.classList.remove('show'); return; }
    const p = document.createElement('div');
    p.textContent = local ? 'Lost the connection to the server; trying again…' : s === 'crashed' ? 'The app crashed.' : 'The app is off.';
    const back = document.createElement('button');
    back.textContent = 'Back home';
    back.onclick = () => DS.home();
    b.append(p, back);
    b.classList.add('show');
  };

  // FB_JPEG: u16 x, u16 y, u16 w, u16 h, then a JPEG. Decoding is async: while
  // one decodes, a newer frame replaces the one waiting, so a slow browser
  // shows fewer frames instead of falling behind.
  let waiting = null, decoding = false;
  function jpeg(buf, off, len) {
    if (len <= 8) return;
    const dv = new DataView(buf, off, 8);
    waiting = { x: dv.getUint16(0, true), y: dv.getUint16(2, true), w: dv.getUint16(4, true), h: dv.getUint16(6, true),
                blob: new Blob([new Uint8Array(buf, off + 8, len - 8)], { type: 'image/jpeg' }), app: DS.app };
    if (!decoding) pump();
  }
  async function pump() {
    decoding = true;
    while (waiting) {
      const f = waiting;
      waiting = null;
      try {
        const bmp = await createImageBitmap(f.blob);
        if (f.app && f.app === DS.app) {
          ctx2d.drawImage(bmp, f.x, f.y, f.w, f.h);
          DS.stats.frames++;
        }
        bmp.close();
      } catch (e) { /* a broken frame: the next one comes */ }
    }
    decoding = false;
  }
  let fpsAt = performance.now(), fpsFrames = 0;
  setInterval(() => {
    const now = performance.now();
    DS.stats.fps = Math.round((DS.stats.frames - fpsFrames) * 1000 / (now - fpsAt));
    fpsAt = now; fpsFrames = DS.stats.frames;
    if (DS.app) DS.emit('fps', DS.stats.fps);
  }, 2000);

  // ---- Start and home --------------------------------------------------------------

  DS.start = (appId, label) => {
    if (!appId) return;
    const old = ws;
    ws = null;
    drop(old);
    const a = DS.apps.find((x) => x.id === appId);
    DS.app = appId;
    DS.label = label || (a && a.name) || appId;
    DS.running = false;
    ending = false;
    DS.view = 'app';
    DS.lastApp = appId;
    if (!appId.startsWith('g_')) history.replaceState(null, '', '?app=' + encodeURIComponent(appId));
    byId('dim').style.opacity = '0';      // our sim sends no PANEL frame: the glass is lit
    byId('banner').classList.remove('show');
    ctx2d.fillStyle = '#000'; ctx2d.fillRect(0, 0, 410, 502);
    ctx2d.fillStyle = '#8b929c'; ctx2d.font = '18px system-ui, sans-serif';
    ctx2d.textAlign = 'center'; ctx2d.textBaseline = 'alphabetic';
    ctx2d.fillText('starting ' + DS.label + '…', 205, 260);
    byId('state').textContent = 'starting…';
    retryMs = 1000;
    sim.connect();
    DS.emit('app', appId);
  };

  DS.restart = () => { if (DS.app) DS.start(DS.app, DS.label); };

  DS.home = () => {
    clearTimeout(homeTimer);
    ending = false;
    const old = ws;
    ws = null;
    drop(old);
    const was = DS.app;
    DS.app = null;
    DS.running = false;
    if (was) history.replaceState(null, '', location.pathname);
    window.setState('off');
    byId('dim').style.opacity = '0';
    DS.emit('home', was);
  };

  // An app that ended: its last frame stays a moment, then home.
  let homeTimer = 0, ending = false;
  function homeSoon() {
    const app = DS.app;
    ending = true;
    clearTimeout(homeTimer);
    homeTimer = setTimeout(() => { if (DS.app === app) DS.home(); }, 700);
  }

  byId('reset').onclick = () => DS.restart();
  byId('power').onclick = () => {
    if (DS.app) DS.restart();
    else DS.start(DS.lastApp || (DS.apps[0] || {}).id);
  };

  DS.loadApps = async () => {
    const s = await (await fetch('/api/status')).json();
    DS.user = s.user || '';
    DS.version = s.version || '';
    const order = ['slang', 'muurbreker', 'blokkenregen', 'schuif', 'fladder', 'tafeltennis', 'kleurgeheugen',
                   'reflex', 'kody_kart', 'krabbel', 'stopwatch'];
    const rank = (a) => { const k = order.indexOf(a.id); return k < 0 ? 99 : k; };
    const mine = DS.apps.filter((a) => a.mine);
    DS.apps = mine.concat(s.apps.slice().sort((a, b) => rank(a) - rank(b) || a.name.localeCompare(b.name)));
    DS.emit('apps', DS.apps);
    return s;
  };

  // ---- Keys and the glass ----------------------------------------------------------

  const editable = (t) => !!(t && t.closest && t.closest('input, textarea, select, [contenteditable], .CodeMirror'));
  // sim.js listens on window and only spares its own console input: keys typed
  // in any other field (the code editor) stop at the document.
  for (const ev of ['keydown', 'keyup']) {
    document.addEventListener(ev, (e) => { if (editable(e.target)) e.stopPropagation(); });
  }
  const KEYS = { t: 'top', T: 'top', b: 'bot', B: 'bot', z: 'top', Z: 'top', x: 'bot', X: 'bot',
                 ArrowUp: 'up', ArrowDown: 'down', ArrowLeft: 'left', ArrowRight: 'right' };
  const EXTRA = { z: 'top', Z: 'top', x: 'bot', X: 'bot' };   // sim.js has T and B; Z and X work too
  window.addEventListener('keydown', (e) => {
    if (editable(e.target) || e.repeat) return;
    if (e.key === 'Escape') { e.preventDefault(); if (DS.app) DS.home(); else DS.emit('key', 'bot'); return; }
    if (DS.app) {
      if (e.key in EXTRA) { e.preventDefault(); button(EXTRA[e.key], true); }
      return;
    }
    if (e.key === 'Enter') { e.preventDefault(); DS.emit('key', 'top'); return; }
    if (e.key in KEYS) DS.emit('key', KEYS[e.key]);
  });
  window.addEventListener('keyup', (e) => {
    if (editable(e.target) || !DS.app || !(e.key in EXTRA)) return;
    e.preventDefault(); button(EXTRA[e.key], false);
  });
  for (const el of document.querySelectorAll('[data-btn]')) {
    el.addEventListener('pointerdown', () => { if (!DS.app) DS.emit('key', el.dataset.btn); });
  }
  // TOP and BOT held together for a second: back home (sim.js's mask: top 1, bot 2).
  let bothSince = 0;
  setInterval(() => {
    if (!DS.app || (mask & 3) !== 3) { bothSince = 0; return; }
    if (!bothSince) bothSince = performance.now();
    else if (performance.now() - bothSince > 1000) { bothSince = 0; DS.home(); }
  }, 100);

  const glass = byId('glass');
  const at = (e) => {
    const r = glass.getBoundingClientRect();
    return [Math.round((e.clientX - r.left) / r.width * 410), Math.round((e.clientY - r.top) / r.height * 502)];
  };
  let touching = false;
  glass.addEventListener('pointerdown', (e) => { if (DS.app) return; touching = true; DS.emit('touch', 'down', ...at(e)); });
  glass.addEventListener('pointermove', (e) => { if (DS.app) return; DS.emit('look', ...at(e)); if (touching) DS.emit('touch', 'move', ...at(e)); });
  for (const ev of ['pointerup', 'pointercancel']) {
    glass.addEventListener(ev, (e) => { if (DS.app || !touching) return; touching = false; DS.emit('touch', 'up', ...at(e)); });
  }

  // ---- The rest: theme first, then the parts --------------------------------------

  function script(src) {
    return new Promise((ok, bad) => {
      const s = document.createElement('script');
      s.src = src; s.onload = ok; s.onerror = () => bad(new Error('could not load ' + src));
      document.head.appendChild(s);
    });
  }
  DS.script = script;
  DS.css = (href) => {
    const l = document.createElement('link');
    l.rel = 'stylesheet'; l.href = href;
    document.head.appendChild(l);
    return l;
  };

  (async () => {
    byId('dim').style.opacity = '0';
    byId('banner').classList.remove('show');
    await script(BASE + 'theme/theme.js');
    const T = window.DOTSIM_THEME;
    if (T.font && T.font.css) DS.css(T.font.css);
    DS.css(BASE + 'shell.css');
    DS.css(BASE + 'theme/theme.css');
    for (const f of ['theme/dash.js', 'english.js', 'shell.js', 'home.js', 'editor.js', 'parts.js']) await script(BASE + f);
    window.setState('off');
    try { await DS.loadApps(); } catch (e) { DS.log('server not reachable'); }
    DS.ready = true;
    DS.emit('ready');
    const want = new URLSearchParams(location.search).get('app');
    if (want && DS.apps.some((a) => a.id === want)) DS.start(want);
  })().catch((e) => console.error(e));
})();
