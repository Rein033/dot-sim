/* The code editor: start from a new app or the code of one on the Dot, change it,
 * Build & run (Ctrl+Enter). Drafts stay in this browser; compiler errors become red
 * lines. Open in Beginner mode; in Advanced it is there, folded. CodeMirror comes
 * from cdnjs and loads after the page is up, so the Dot never waits for it. */
(function () {
  'use strict';
  const DS = window.DotSim, esc = DS.esc;
  const CM = 'https://cdnjs.cloudflare.com/ajax/libs/codemirror/5.65.16/';

  const sec = document.createElement('section');
  sec.id = 'dotsim-editor';
  sec.innerHTML =
    '<details id="ds-fold"><summary>Code</summary>' +
    '<div class="bar"><select id="ds-base" title="Start from"></select>' +
    '<button id="ds-play" class="ds-yellow" title="Ctrl+Enter">Build &amp; run</button>' +
    '<button id="ds-reset" class="ds-ghost" title="Throw away your changes">Reset</button>' +
    '<button id="ds-dl" class="ds-ghost" title="Save app_main.c">Download</button></div>' +
    '<div class="tabs"><button id="ds-tab-main" class="on">app_main.c</button><button id="ds-tab-kk">kk.h (read only)</button>' +
    '<span class="hint">Your edits are kept in this browser.</span></div>' +
    '<textarea id="ds-code"></textarea><ul id="ds-problems"></ul></details>';
  document.getElementById('dotsim-aside').after(sec);
  const fold = document.getElementById('ds-fold');
  const foldFor = (m) => { fold.open = m === 'beginner'; };
  foldFor(DS.mode);
  DS.listen('mode', foldFor);

  const $ = (id) => document.getElementById(id);
  const baseSel = $('ds-base'), playBtn = $('ds-play'), probs = $('ds-problems');
  const store = {
    get: (k) => DS.store.get(k),
    set: (k, v) => DS.store.set(k, v),
    del(k) { try { localStorage.removeItem(k); } catch (e) { /* not kept */ } },
  };
  let cm = null, mainDoc = null, kkDoc = null, base = 'new', pristine = '', marks = [];
  const draftKey = () => 'kode-code-' + base;

  function showTab(kk) {
    $('ds-tab-main').classList.toggle('on', !kk);
    $('ds-tab-kk').classList.toggle('on', kk);
    cm.swapDoc(kk ? kkDoc : mainDoc);
    cm.setOption('readOnly', kk);
  }

  async function loadSource(id) {
    const r = await (await fetch('/api/source/' + encodeURIComponent(id))).json();
    base = id; pristine = r.code;
    kkDoc.setValue(r.kk_h);
    showTab(false);
    mainDoc.setValue(store.get(draftKey()) ?? r.code);
    mainDoc.clearHistory();
    clearMarks();
    store.set('kode-base', id);
  }
  async function loadBases() {
    const r = await (await fetch('/api/sources')).json();
    baseSel.innerHTML = '';
    const add = (v, t) => { const o = document.createElement('option'); o.value = v; o.textContent = t; baseSel.appendChild(o); };
    add('new', 'New app (starter)');
    for (const a of r.apps) add(a.id, 'Code of ' + a.name);
    const want = new URLSearchParams(location.search).get('code') || store.get('kode-base') || 'new';
    baseSel.value = [...baseSel.options].some((o) => o.value === want) ? want : 'new';
    await loadSource(baseSel.value);
  }

  function clearMarks() {
    for (const m of marks) mainDoc.removeLineClass(m.line, 'background', m.cls);
    marks = []; probs.innerHTML = '';
  }
  function markProblems(lines) {
    clearMarks();
    const seen = new Set();
    for (const l of lines) {
      const m = /app_main\.c:(\d+):(\d+):\s*(fatal error|error|warning):\s*(.*)$/.exec(l);
      if (!m) continue;
      const key = m[1] + m[3] + m[4];
      if (seen.has(key)) continue;
      seen.add(key);
      const line = parseInt(m[1], 10) - 1, warn = m[3] === 'warning', cls = warn ? 'cm-warn-line' : 'cm-err-line';
      mainDoc.addLineClass(line, 'background', cls); marks.push({ line, cls });
      const li = document.createElement('li');
      if (warn) li.className = 'warn';
      li.textContent = 'line ' + m[1] + ': ' + m[3] + ': ' + m[4];
      li.addEventListener('click', () => { showTab(false); cm.focus(); cm.setCursor({ line, ch: parseInt(m[2], 10) - 1 }); cm.scrollIntoView(null, 120); });
      probs.appendChild(li);
    }
  }

  async function play() {
    if (playBtn.disabled || !cm) return;
    showTab(false);
    playBtn.disabled = true; clearMarks();
    DS.status('Building your code…');
    DS.log('— building your code');
    let r;
    try {
      r = await (await fetch('/api/play', { method: 'POST', headers: { 'Content-Type': 'application/json' },
                                           body: JSON.stringify({ base, code: mainDoc.getValue() }) })).json();
    } catch (e) { r = { error: 'server not reachable' }; }
    if (r.error) { DS.log('error: ' + r.error); DS.status('Error: ' + esc(r.error)); playBtn.disabled = false; return; }
    const name = (/KODE_APP_MANIFEST\(\s*"([^"]+)"/.exec(mainDoc.getValue()) || [, 'your app'])[1];
    if (r.job) {
      const all = [];
      const j = await DS.follow(r.job, (st) => DS.status(st === 'waiting' ? 'Waiting for a free builder…' : 'Compiling…'), all);
      markProblems(all);
      if (j.state !== 'done') {
        playBtn.disabled = false;
        DS.status(probs.children.length ? 'Your code has errors: see the red lines.' : 'Build failed, see the console.');
        return;
      }
    } else {
      DS.log('— same code as before, no rebuild needed');
    }
    playBtn.disabled = false;
    DS.apps = [{ id: r.app, name, color: DS.theme().colours.yellow, desc: 'your code', mine: true }]
      .concat(DS.apps.filter((a) => !a.mine));
    DS.emit('apps', DS.apps);
    DS.notify('Built: ' + name, 0);
    DS.start(r.app, name);
  }

  async function init() {
    DS.css(CM + 'codemirror.min.css');
    await DS.script(CM + 'codemirror.min.js');
    for (const f of ['mode/clike/clike.min.js', 'addon/edit/matchbrackets.min.js', 'addon/edit/closebrackets.min.js']) {
      await DS.script(CM + f);
    }
    cm = CodeMirror.fromTextArea($('ds-code'), {
      mode: 'text/x-csrc', lineNumbers: true, indentUnit: 4, tabSize: 4, indentWithTabs: false,
      matchBrackets: true, autoCloseBrackets: true,
      extraKeys: { 'Ctrl-Enter': () => play(), 'Cmd-Enter': () => play(),
                   Tab: (c) => (c.somethingSelected() ? c.indentSelection('add') : c.replaceSelection('    ')) },
    });
    mainDoc = cm.getDoc();
    kkDoc = CodeMirror.Doc('', 'text/x-csrc');
    fold.addEventListener('toggle', () => { if (fold.open) cm.refresh(); });
    $('ds-tab-main').addEventListener('click', () => showTab(false));
    $('ds-tab-kk').addEventListener('click', () => showTab(true));
    baseSel.addEventListener('change', () => loadSource(baseSel.value));
    let saveT = 0;
    mainDoc.on('change', () => {
      clearTimeout(saveT);
      saveT = setTimeout(() => {
        const v = mainDoc.getValue();
        if (v === pristine) store.del(draftKey()); else store.set(draftKey(), v);
      }, 400);
    });
    $('ds-reset').addEventListener('click', () => {
      if (mainDoc.getValue() !== pristine && !confirm('Throw away your changes to this code?')) return;
      store.del(draftKey()); mainDoc.setValue(pristine); clearMarks();
    });
    $('ds-dl').addEventListener('click', () => {
      const a = document.createElement('a');
      a.href = URL.createObjectURL(new Blob([mainDoc.getValue()], { type: 'text/x-c' }));
      a.download = (base === 'new' ? 'my_app' : base) + '_app_main.c'; a.click();
      setTimeout(() => URL.revokeObjectURL(a.href), 1000);
    });
    playBtn.addEventListener('click', play);
    await loadBases();
    DS.editor = cm;
    DS.emit('editor', cm);
  }
  DS.theme = () => window.DOTSIM_THEME;
  init().catch((e) => DS.log('could not load the code editor: ' + e.message));
})();
