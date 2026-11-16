/* The board's parts, one card each in Advanced (the cards sit under the motion card).
 * Page <-> server: JSON text on the KD socket, {"<part>": ...} (see server.py).
 *
 *   microSD: kode-sim's own "SD inserted" box (#sdin) puts the card in and out
 *   ({"sd": bool}); this card lists the card's files, uploads (700 KB each, 16 MB
 *   in all) and downloads them. Every file name is text, never markup.
 */
(function () {
  'use strict';
  const DS = window.DotSim;
  const byId = (id) => document.getElementById(id);
  const FILE_MAX = 700 * 1024, TOTAL_MAX = 16 * 1024 * 1024;
  const size = (n) => n >= 1048576 ? (n / 1048576).toFixed(1) + ' MB' : n >= 1024 ? (n / 1024).toFixed(1) + ' KB' : n + ' B';
  const send = (j) => window.sendJson(j);

  // ---- microSD ----
  const card = document.createElement('div');
  card.className = 'card';
  card.id = 'sdcard';
  const h = document.createElement('h2');
  h.textContent = 'microSD card';
  const up = document.createElement('div');
  up.className = 'row';
  const pick = document.createElement('input');
  pick.type = 'file'; pick.id = 'sdfile'; pick.multiple = true; pick.disabled = true;
  pick.title = 'Put files on the card: 700 KB each, 16 MB in all';
  up.append(pick);
  const info = document.createElement('div');
  info.className = 'row keys'; info.id = 'sdinfo';
  const list = document.createElement('div');
  list.id = 'sdlist';
  card.append(h, up, info, list);
  byId('motioncard').after(card);

  let files = [];
  function say(text) { info.textContent = text; }
  function render() {
    const used = files.reduce((n, f) => n + f.size, 0);
    say(DS.running ? 'The card holds ' + size(used) + ' of 16 MB. Tick "SD inserted" so the app sees it.'
                   : 'Start an app to use the card.');
    pick.disabled = !DS.running;
    list.replaceChildren();
    for (const f of files) {
      const row = document.createElement('div');
      row.className = 'row';
      const name = document.createElement('span');
      name.style.flex = '1'; name.style.overflowWrap = 'anywhere';
      name.textContent = f.name + ' · ' + size(f.size);
      const get = document.createElement('button');
      get.className = 'sd-get'; get.textContent = 'Download';
      get.onclick = () => send({ sd_get: { name: f.name } });
      row.append(name, get);
      list.append(row);
    }
  }

  function b64of(file) {
    return new Promise((ok, bad) => {
      const r = new FileReader();
      r.onload = () => ok(String(r.result).split(',')[1] || '');
      r.onerror = () => bad(r.error);
      r.readAsDataURL(file);
    });
  }
  pick.addEventListener('change', async () => {
    let held = files.reduce((n, f) => n + f.size, 0);
    for (const f of Array.from(pick.files)) {
      if (f.size > FILE_MAX) { say(f.name + ' is bigger than 700 KB.'); continue; }
      if (held + f.size > TOTAL_MAX) { say('The card would hold more than 16 MB.'); continue; }
      held += f.size;
      try { send({ sd_put: { name: f.name, b64: await b64of(f) } }); } catch (e) { say('Could not read ' + f.name + '.'); }
    }
    pick.value = '';
  });

  DS.listen('json', (j) => {
    if (j.sd_files) { files = j.sd_files; render(); }
    if (j.sd_error) say(String(j.sd_error));
    if (j.sd_file) {
      const bin = atob(j.sd_file.b64), u = new Uint8Array(bin.length);
      for (let i = 0; i < bin.length; i++) u[i] = bin.charCodeAt(i);
      const a = document.createElement('a');
      a.href = URL.createObjectURL(new Blob([u]));
      a.download = j.sd_file.name;
      document.body.append(a);
      a.click();
      a.remove();
      setTimeout(() => URL.revokeObjectURL(a.href), 10000);
    }
  });
  // A new session starts with the card out: tell it what the box says, and ask for the list.
  DS.listen('running', () => { send({ sd: byId('sdin').checked }); send({ sd_ls: {} }); render(); });
  DS.listen('home', () => { files = []; render(); });
  render();
})();
