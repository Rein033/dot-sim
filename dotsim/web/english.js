/* kode-sim's page in English. Its Spanish stays in web/ (upstream's files, merged
 * as they are); this swaps the text at load and keeps swapping what sim.js writes
 * later (state, units, the compass). It also hides the controls the fast engine
 * has nothing behind yet. */
(function () {
  'use strict';
  const $id = (id) => document.getElementById(id);

  // Whole texts, as sim.js and index.html have them (trimmed).
  const WORDS = {
    'conectando…': 'connecting…', 'encendido': 'running', 'apagado': 'off', 'colgado': 'crashed', 'en espera': 'waiting',
    'Encendido': 'Running', 'Encender': 'Start', 'Más': 'More', 'Menos': 'Less', 'Reiniciar': 'Restart',
    'Haz clic en el dispositivo para activar el sonido': 'Click the device to turn the sound on',
    'Sonido activo': 'Sound on', 'Micrófono del Mac': 'Microphone', 'Instalar app (.bin)': 'Install app (.bin)',
    'Batería': 'Battery', 'USB conectado': 'USB connected', 'SD insertada': 'SD card in',
    'KodeOS lo sigue como a una batería real: calcula el % por la tensión y lo mueve como mucho 2 puntos cada 2 s.':
      'The app reads it through the Dot\'s battery gauge, as it would on a real one.',
    'Movimiento (IMU y brújula)': 'Motion (IMU and compass)', 'Cabeceo': 'Pitch', 'Alabeo': 'Roll', 'Rumbo': 'Heading',
    'Imán': 'Magnet', 'Sacudir': 'Shake', 'Plano': 'Flat', 'sin imán': 'no magnet',
    'Ordenador (USB)': 'Computer (USB)', 'Consola': 'Console', 'sin cable': 'no cable', 'Conectar cable': 'Plug in',
    'Sacar cable': 'Unplug', 'Expulsar': 'Eject', 'Lo que hace el ordenador': 'What the computer does',
  };
  const COMPASS = { SO: 'SW', O: 'W', NO: 'NW' };
  const TITLES = {
    settings: 'Battery, motion and the console', reset: 'Start the app again', power: 'Start the app',
    more: 'More controls', micin: '', mshake: 'A shake of 1.2 s', mflat: 'Flat, facing north, no magnet',
    mmag: 'How far a magnet is from the back, in mm; at the end, none',
  };

  function swap(node) {
    const raw = node.nodeValue, t = raw.trim();
    if (!t) return;
    let en = WORDS[t];
    if (en === undefined) {
      const m = /^(-?\d+)° (SO|O|NO)$/.exec(t);
      if (m) en = m[1] + '° ' + COMPASS[m[2]];
    }
    if (en !== undefined && en !== t) node.nodeValue = raw.replace(t, en);
  }
  const skip = (n) => { const e = n.nodeType === 1 ? n : n.parentElement; return !e || !!e.closest('#log, #usblog, #dotsim-editor, script, style'); };
  function walk(root) {
    if (skip(root)) return;
    if (root.nodeType === 3) { swap(root); return; }
    const w = document.createTreeWalker(root, NodeFilter.SHOW_TEXT);
    for (let n = w.nextNode(); n; n = w.nextNode()) if (!skip(n)) swap(n);
  }

  document.querySelector('#settings .open').textContent = '⚙ Settings';
  document.querySelector('#settings .close').textContent = '✕ Close';
  for (const [id, title] of Object.entries(TITLES)) { const e = $id(id); if (e) e.title = title; }
  for (const e of document.querySelectorAll('[title]')) if (/[áéíóúñ¿]|\b(el|la|del|los)\b/i.test(e.title)) e.title = '';
  const keys = document.querySelector('.side > .keys');
  if (keys) keys.innerHTML = 'Keys: <kbd>T</kbd> TOP · <kbd>B</kbd> BOT · <kbd>←↑→↓</kbd> d-pad · click the glass = finger · ' +
    '<kbd>Esc</kbd> or TOP+BOT 1 s = home';
  walk(document.body);
  new MutationObserver((list) => {
    for (const m of list) {
      if (m.type === 'characterData') walk(m.target);
      else for (const n of m.addedNodes) walk(n);
    }
  }).observe(document.body, { subtree: true, childList: true, characterData: true });

  // Nothing behind these in the fast engine (yet).
  for (const id of ['micin', 'app', 'sdin']) { const e = $id(id); if (e) e.closest('.row').classList.add('ds-unsupported'); }
  $id('usbcard').classList.add('ds-unsupported');
})();
