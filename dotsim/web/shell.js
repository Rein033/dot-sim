/* Dot Sim's frame around kode-sim's page: the theme's colours and names, the header
 * (mascot, name, mode switch, Docs, kode-sim's settings button), our card next to the
 * device (what is happening, Home, Restart, the keys, building from Git when logged
 * in) and the footer (the fine print and the running commit). */
(function () {
  'use strict';
  const DS = window.DotSim, T = window.DOTSIM_THEME, root = document.documentElement;
  const esc = (t) => String(t).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
  DS.esc = esc;

  for (const [k, v] of Object.entries(T.colours)) root.style.setProperty('--ds-' + k.replace(/[A-Z]/g, (c) => '-' + c.toLowerCase()), v);
  root.style.setProperty('--ds-font', '"' + T.font.family + '"');
  root.lang = 'en';
  document.title = T.name;
  if (T.icon) {
    const l = document.createElement('link');
    l.rel = 'icon'; l.href = T.icon;
    document.head.appendChild(l);
  }

  // ---- header ----
  const top = document.createElement('header');
  top.id = 'dotsim-top';
  top.innerHTML = '<canvas id="ds-logo" width="80" height="88" title="' + esc(T.mascot) + '"></canvas>' +
    '<a class="brand" href="/">' + esc(T.name) + '</a><span class="pill">Unofficial</span><span class="sp"></span>' +
    '<span class="ds-modes" role="group" aria-label="Mode">' +
    '<button data-mode="beginner" title="The apps, the code and the battery">Beginner</button>' +
    '<button data-mode="advanced" title="Also motion and the console">Advanced</button></span>' +
    '<a class="ds-btn ds-ghost" href="' + esc(T.docs) + '">Docs</a>';
  top.appendChild(document.getElementById('settings'));
  document.body.prepend(top);
  if (window.Dash) {
    const lx = document.getElementById('ds-logo').getContext('2d');
    (function f(t) { lx.clearRect(0, 0, 80, 88); Dash.draw(lx, { x: 40, y: 85, s: 0.36, t, mood: 'idle' }); requestAnimationFrame(f); })(0);
  }

  function applyMode(m) {
    root.classList.toggle('ds-beginner', m === 'beginner');
    root.classList.toggle('ds-advanced', m === 'advanced');
    for (const b of top.querySelectorAll('[data-mode]')) b.classList.toggle('on', b.dataset.mode === m);
  }
  for (const b of top.querySelectorAll('[data-mode]')) b.addEventListener('click', () => DS.setMode(b.dataset.mode));
  DS.listen('mode', applyMode);
  applyMode(DS.mode);

  // ---- our card, next to the device ----
  const aside = document.createElement('aside');
  aside.id = 'dotsim-aside';
  aside.innerHTML =
    '<div id="dotsim-status">Pick an app on the Dot.</div>' +
    '<div class="row"><button id="ds-home" title="Back to the apps (Esc)">Home</button>' +
    '<button id="ds-restart" class="ds-ghost" title="Start the app again" disabled>Restart</button></div>' +
    '<div id="ds-buildbox" hidden><label for="ds-git">Build an app from Git</label>' +
    '<div class="row"><input id="ds-git" type="text" placeholder="https://github.com/kodediy/kode-app-template">' +
    '<button id="ds-build">Build</button></div></div>' +
    '<div class="keys">Keys: <kbd>←</kbd><kbd>↑</kbd><kbd>→</kbd><kbd>↓</kbd> d-pad, <kbd>T</kbd> or <kbd>Z</kbd> TOP, ' +
    '<kbd>B</kbd> or <kbd>X</kbd> BOT. Click or tap the glass to touch. On the home screen, TOP or a tap opens an app; ' +
    'swipe or <kbd>←</kbd>/<kbd>→</kbd> for more. Hold TOP and BOT for a second (or <kbd>Esc</kbd>) to go back home.</div>';
  document.getElementById('stage').after(aside);
  const statusEl = document.getElementById('dotsim-status');
  DS.status = (html) => { if (statusEl.innerHTML !== html) statusEl.innerHTML = html; };
  document.getElementById('ds-home').addEventListener('click', () => DS.home());
  const restart = document.getElementById('ds-restart');
  restart.addEventListener('click', () => DS.restart());
  DS.listen('app', () => { restart.disabled = false; DS.status('Starting <b>' + esc(DS.label) + '</b>…'); });
  DS.listen('home', () => { restart.disabled = true; });
  DS.listen('running', () => DS.status('Running: <b>' + esc(DS.label) + '</b>'));
  DS.listen('fps', (fps) => { if (DS.running) DS.status('Running: <b>' + esc(DS.label) + '</b> · ' + fps + ' fps'); });
  DS.listen('error', (s) => DS.status('Error: ' + esc(s)));

  // ---- footer ----
  const foot = document.createElement('footer');
  foot.id = 'dotsim-foot';
  foot.innerHTML = '<span>' + esc(T.legal.text) + '</span> Source: <a href="' + esc(T.legal.source) + '">' +
    esc(T.legal.source.replace(/^https:\/\//, '')) + '</a><span id="ds-ver"></span>.';
  document.querySelector('main').after(foot);
  DS.listen('apps', () => {
    document.getElementById('ds-buildbox').hidden = !DS.user;
    if (DS.version) {
      document.getElementById('ds-ver').innerHTML = ' at <a href="' + esc(T.legal.source) + '/commit/' +
        encodeURIComponent(DS.version) + '">' + esc(DS.version) + '</a>';
    }
  });

  // ---- build from Git (logged in only) ----
  const gitIn = document.getElementById('ds-git'), buildBtn = document.getElementById('ds-build');
  async function build() {
    const repo = gitIn.value.trim();
    if (!repo) return;
    buildBtn.disabled = true;
    DS.status('Building…');
    DS.log('— building: ' + repo);
    let r;
    try {
      r = await (await fetch('/api/build', { method: 'POST', headers: { 'Content-Type': 'application/json' },
                                            body: JSON.stringify({ repo }) })).json();
    } catch (e) { r = { error: 'server not reachable' }; }
    if (r.error) { DS.log('error: ' + r.error); DS.status('Error: ' + esc(r.error)); buildBtn.disabled = false; return; }
    const j = await DS.follow(r.job, (st) => DS.status('Build: ' + st + '…'));
    buildBtn.disabled = false;
    if (j.state === 'done') { await DS.loadApps(); DS.log('— built ' + j.app); DS.start(j.app); }
    else DS.status('Build failed, see the console.');
  }
  buildBtn.addEventListener('click', build);
  gitIn.addEventListener('keydown', (e) => { if (e.key === 'Enter') build(); });

  // A build job's lines to the console until it is done; onState('waiting'|'building') meanwhile.
  DS.follow = async (job, onState, lines) => {
    let next = 0;
    for (;;) {
      await new Promise((res) => setTimeout(res, 1000));
      let j;
      try { j = await (await fetch('/api/build/' + job + '?since=' + next)).json(); } catch (e) { continue; }
      for (const line of j.lines) { DS.log(line); if (lines) lines.push(line); }
      next = j.next;
      if (j.state === 'building' || j.state === 'waiting') { onState(j.state); continue; }
      return j;
    }
  };
})();
