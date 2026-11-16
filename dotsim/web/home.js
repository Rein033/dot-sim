/* The home screen on the glass while no app runs: the theme's mascot (Dash) with
 * the time and the date, and TOP, a d-pad key or a tap for the app grid. In the
 * grid the d-pad moves, TOP or a tap opens, a swipe or ←/→ turns the page, BOT
 * goes back to the face. After 30 s in the grid, or on its own, Dash returns. */
(function () {
  'use strict';
  const DS = window.DotSim, T = window.DOTSIM_THEME;
  const cv = document.getElementById('screen'), ctx = cv.getContext('2d');
  const FONT = T.font.family + ', system-ui, sans-serif';
  const PALETTE = ['#028AC2', '#46BE5A', '#F58228', '#9658DC', '#C9303D', '#3CC8DC', '#FAC828', '#F06EAA'];
  const L = { sel: 0, per: 9, down: null, last: performance.now(), wakeAt: 0, look: null, lookAt: 0,
              note: null, noteAt: 0, min: -1, clockAt: -1e9 };
  const COLS = 3, TW = 92, TH = 92, GX = 22, X0 = (410 - (3 * 92 + 2 * 22)) / 2, Y0 = 96, RH = 120;
  const DAYS = ['Sunday', 'Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday'];
  const MONTHS = ['January', 'February', 'March', 'April', 'May', 'June', 'July', 'August', 'September',
                  'October', 'November', 'December'];
  const esc = DS.esc;

  const tileColor = (a, i) => a.color || PALETTE[i % PALETTE.length];
  function light(hex) {
    const n = parseInt(hex.slice(1), 16);
    return ((n >> 16) * 299 + ((n >> 8) & 255) * 587 + (n & 255) * 114) / 1000 > 170;
  }
  function rrect(x, y, w, h, r) {
    ctx.beginPath(); ctx.moveTo(x + r, y); ctx.arcTo(x + w, y, x + w, y + h, r); ctx.arcTo(x + w, y + h, x, y + h, r);
    ctx.arcTo(x, y + h, x, y, r); ctx.arcTo(x, y, x + w, y, r); ctx.closePath();
  }
  const tileAt = (slot) => ({ x: X0 + (slot % COLS) * (TW + GX), y: Y0 + Math.floor(slot / COLS) * RH });
  const hhmm = (d) => String(d.getHours()).padStart(2, '0') + ':' + String(d.getMinutes()).padStart(2, '0');

  function mood(t) {
    if (t - L.noteAt < 4500 && L.note) return 'notify';
    if (t - L.wakeAt < 1400) return 'wake';
    if (t - L.last > 45000) return 'sleep';
    if (t - L.clockAt < 3500) return 'clock';
    return 'idle';
  }

  function drawFace(t) {
    const now = new Date();
    if (now.getMinutes() !== L.min) { if (L.min >= 0) L.clockAt = t; L.min = now.getMinutes(); }
    const m = mood(t), dim = m === 'sleep';
    const g = ctx.createLinearGradient(0, 0, 0, 502);
    g.addColorStop(0, '#1a1712'); g.addColorStop(1, '#050505');
    ctx.fillStyle = g; ctx.fillRect(0, 0, 410, 502);
    ctx.textAlign = 'center'; ctx.textBaseline = 'alphabetic';
    ctx.fillStyle = dim ? '#8A8A8A' : '#F4EFE7'; ctx.font = '700 76px ' + FONT;
    ctx.fillText(hhmm(now), 205, 128);
    ctx.fillStyle = T.colours.yellow; ctx.font = '500 19px ' + FONT;
    ctx.fillText(DAYS[now.getDay()] + ' ' + now.getDate() + ' ' + MONTHS[now.getMonth()], 205, 160);
    ctx.save(); ctx.globalAlpha = dim ? 0.55 : 1;
    Dash.draw(ctx, { x: 205, y: 448, s: 1.0, t, mood: m, time: hhmm(now), note: L.note,
                     look: (t - L.lookAt < 2500) ? L.look : null });
    ctx.restore();
    if (!dim && t - L.last < 8000) {
      ctx.fillStyle = 'rgba(232,232,232,.55)'; ctx.font = '13px ' + FONT;
      ctx.fillText('TOP or tap for apps', 205, 186);
    }
    DS.status(dim ? esc(T.mascot) + ' is asleep. Press a button to wake him.' : 'Home · press TOP or tap the screen for the apps');
  }

  function drawGrid() {
    const g = ctx.createLinearGradient(0, 0, 0, 502);
    g.addColorStop(0, '#161b23'); g.addColorStop(1, '#05070a');
    ctx.fillStyle = g; ctx.fillRect(0, 0, 410, 502);
    ctx.textAlign = 'center'; ctx.textBaseline = 'alphabetic';
    ctx.fillStyle = '#e8eaed'; ctx.font = '600 26px ' + FONT;
    ctx.fillText(hhmm(new Date()), 205, 64);
    const apps = DS.apps;
    if (!apps.length) {
      ctx.fillStyle = '#8b929c'; ctx.font = '18px ' + FONT;
      ctx.fillText(DS.ready ? 'no apps built yet' : 'loading apps…', 205, 260);
      DS.status(DS.ready ? 'No apps built yet.' : 'Loading the apps…');
      return;
    }
    L.sel = Math.min(L.sel, apps.length - 1);
    const pages = Math.ceil(apps.length / L.per), page = Math.floor(L.sel / L.per);
    for (let slot = 0; slot < L.per; slot++) {
      const i = page * L.per + slot, a = apps[i];
      if (!a) break;
      const { x, y } = tileAt(slot), col = tileColor(a, i);
      if (i === L.sel) { ctx.strokeStyle = '#fff'; ctx.lineWidth = 3; rrect(x - 5, y - 5, TW + 10, TH + 10, 26); ctx.stroke(); }
      ctx.fillStyle = col; rrect(x, y, TW, TH, 22); ctx.fill();
      if (a.img && a.img.complete && a.img.naturalWidth) {
        ctx.save(); rrect(x, y, TW, TH, 22); ctx.clip();
        ctx.imageSmoothingEnabled = true; ctx.imageSmoothingQuality = 'high';
        ctx.drawImage(a.img, x, y, TW, TH); ctx.restore();
      } else {
        ctx.fillStyle = light(col) ? '#14171c' : '#fff';
        ctx.font = (a.mine ? '700 28px ' : '700 40px ') + FONT; ctx.textBaseline = 'middle';
        ctx.fillText(a.mine ? '</>' : a.name.charAt(0).toUpperCase(), x + TW / 2, y + TH / 2 + 2);
      }
      ctx.textBaseline = 'alphabetic';
      ctx.fillStyle = i === L.sel ? '#fff' : '#aeb5bf'; ctx.font = '13px ' + FONT;
      let n = a.name;
      while (ctx.measureText(n).width > TW + 18 && n.length > 3) n = n.slice(0, -2) + '…';
      ctx.fillText(n, x + TW / 2, y + TH + 18);
    }
    if (pages > 1) {
      for (let p = 0; p < pages; p++) {
        ctx.fillStyle = p === page ? '#e8eaed' : '#4a515c';
        ctx.beginPath(); ctx.arc(205 + (p - (pages - 1) / 2) * 16, 470, 4, 0, Math.PI * 2); ctx.fill();
      }
    }
    const a = apps[L.sel];
    DS.status('Home · <b>' + esc(a.name) + '</b>' + (a.desc ? ' · ' + esc(a.desc) : ''));
  }

  function draw() {
    if (DS.app) return;
    if (DS.view === 'grid') drawGrid(); else drawFace(performance.now());
  }

  // Dash moves: the face is drawn every frame; back to it after 30 s in the grid.
  (function loop() {
    if (!DS.app) {
      const t = performance.now();
      if (DS.view === 'grid' && t - L.last > 30000) DS.view = 'face';
      if (DS.view !== 'grid') DS.view = 'face';
      if (DS.view === 'face') drawFace(t);
    }
    requestAnimationFrame(loop);
  })();
  setInterval(() => { if (!DS.app && DS.view === 'grid') drawGrid(); }, 15000);

  // A press after 45 s idle only wakes Dash.
  function poke() {
    const t = performance.now(), asleep = t - L.last > 45000;
    L.last = t;
    if (asleep) L.wakeAt = t;
    return asleep;
  }
  function open(i) { const a = DS.apps[i]; if (a) { L.sel = i; L.last = performance.now(); DS.start(a.id, a.name); } }

  DS.listen('key', (name) => {
    if (DS.app) return;
    if (poke() && DS.view === 'face') return;
    if (DS.view !== 'grid') {
      if (name !== 'bot') { DS.view = 'grid'; draw(); }
      return;
    }
    if (name === 'bot') { DS.view = 'face'; return; }
    const n = DS.apps.length;
    if (!n) return;
    const page = Math.floor(L.sel / L.per);
    if (name === 'left') L.sel = (L.sel - 1 + n) % n;
    else if (name === 'right') L.sel = (L.sel + 1) % n;
    else if (name === 'up') L.sel = Math.max(page * L.per, L.sel - COLS);
    else if (name === 'down') L.sel = Math.min(n - 1, page * L.per + L.per - 1, L.sel + COLS);
    else if (name === 'top') { open(L.sel); return; }
    draw();
  });

  DS.listen('touch', (kind, x, y) => {
    if (kind === 'down') { L.down = { x, y }; return; }
    if (kind !== 'up' || !L.down) return;
    const start = L.down;
    L.down = null;
    if (DS.view !== 'grid') { if (!poke()) { DS.view = 'grid'; draw(); } return; }
    L.last = performance.now();
    const dx = x - start.x, dy = y - start.y;
    const pages = Math.ceil(DS.apps.length / L.per), page = Math.floor(L.sel / L.per);
    if (Math.abs(dx) > 60 && Math.abs(dx) > Math.abs(dy)) {
      const np = Math.min(pages - 1, Math.max(0, page + (dx < 0 ? 1 : -1)));
      if (np !== page) { L.sel = np * L.per; draw(); }
      return;
    }
    for (let slot = 0; slot < L.per; slot++) {
      const i = page * L.per + slot;
      if (!DS.apps[i]) break;
      const t = tileAt(slot);
      if (start.x >= t.x - 8 && start.x <= t.x + TW + 8 && start.y >= t.y - 8 && start.y <= t.y + TH + 24) { open(i); return; }
    }
  });

  DS.listen('look', (x, y) => { L.look = { x, y }; L.lookAt = performance.now(); });
  DS.listen('home', (was) => { L.last = performance.now(); DS.view = was ? 'grid' : 'face'; draw(); });
  DS.listen('apps', (apps) => {
    for (const a of apps) {
      if (a.icon && !a.img) { a.img = new Image(); a.img.onload = draw; a.img.src = '/api/icon/' + a.id; }
    }
    draw();
  });
  // A note from Dash on the face (the editor's "Built: ..."), with the grid's first tile chosen.
  DS.notify = (text, sel) => { L.note = text; L.noteAt = performance.now(); if (sel !== undefined) L.sel = sel; };
})();
