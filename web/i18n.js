// The page's words, in Spanish and in English, and which of them it speaks.
//
// Static text and attributes name their string with data-i18n (the text),
// data-i18n-html (the text, with markup: only strings written here),
// data-i18n-title and data-i18n-placeholder; sim.js asks for the rest with
// t(key, ...args). A string is text, or a function of its arguments.
// Loaded before sim.js, which does the re-labelling (setLang).

const STRINGS = {
  es: {
    // the window and the first card
    "settings.title": "Batería, USB, tarjeta SD, movimiento, instalar una app y la consola",
    "settings.open": "⚙ Ajustes",
    "settings.close": "✕ Cerrar",
    "more": "Más",
    "less": "Menos",
    "more.title": "Batería, USB, tarjeta SD, instalar una app y la consola",
    "reset.title": "Reinicia el chip, como el circuito de reset de la placa (abajo + izquierda 1 s)",
    "power.on": "Encender",
    "power.running": "Encendido",
    "restart": "Reiniciar",
    "state.connecting": "conectando…",
    "state.running": "encendido",
    "state.off": "apagado",
    "state.crashed": "colgado",
    "state.busy": "en espera",
    "banner.off": "Apagado. Pulsa el botón inferior para encenderlo.",
    "banner.busy": "Todos los simuladores están en uso. Esta página lo vuelve a intentar sola cada 15 s.",
    "banner.crashed": "El firmware se ha colgado.",
    "banner.offline": "Sin conexión con kode-sim; reintentando…",
    "sound.hint": "Haz clic en el dispositivo para activar el sonido",
    "sound.on": "Sonido activo",
    "mic.title": "Lo que oye el micrófono del Mac llega al del Kode Dot. Apagado, el Kode Dot oye solo su altavoz y su ruido de fondo",
    "mic.label": "Micrófono del Mac",
    "app.title": "Una app compilada para el simulador: kode-sim build tu-app imprime su .bin",
    "app.label": "Instalar app (.bin)",
    // battery
    "battery.h": "Batería",
    "battery.usb": "USB conectado",
    "battery.sd": "SD insertada",
    "battery.note": "KodeOS lo sigue como a una batería real: calcula el % por la tensión y lo mueve como mucho 2 puntos cada 2 s.",
    // motion
    "motion.h": "Movimiento (IMU y brújula)",
    "motion.pitch": "Cabeceo",
    "motion.roll": "Alabeo",
    "motion.heading": "Rumbo",
    "motion.magnet": "Imán",
    "motion.magnet.title": "Distancia del imán a la parte trasera, en mm; al final, sin imán",
    "motion.nomagnet": "sin imán",
    "motion.shake": "Sacudir",
    "motion.shake.title": "Una sacudida de 1,2 s",
    "motion.flat": "Plano",
    "motion.flat.title": "Plano, mirando al norte, sin imán",
    "motion.compass": ["N", "NE", "E", "SE", "S", "SO", "O", "NO"],
    // the computer on the USB cable
    "usb.h": "Ordenador (USB)",
    "usb.stage.no_cable": "sin cable",
    "usb.stage.waiting": "cable conectado: nada responde",
    "usb.stage.enumerating": "enumerando…",
    "usb.stage.mounted": "montado",
    "usb.stage.keyboard": "teclado",
    "usb.stage.ejected": "expulsado: ya se puede sacar el cable",
    "usb.stage.failed": "falló",
    "usb.plug": "Conectar cable",
    "usb.unplug": "Sacar cable",
    "usb.plug.title": "Conecta o desconecta el cable USB entre el Kode Dot y este ordenador simulado",
    "usb.eject": "Expulsar",
    "usb.eject.title": "Expulsar el disco, como el sistema operativo antes de sacar el cable",
    "usb.device": "dispositivo",
    "usb.volume": (label, free, total, protectedDisk) =>
      `${label || "sin nombre"} · ${free} libres de ${total}${protectedDisk ? " · protegido contra escritura" : ""}`,
    "usb.file.title": "Leer el archivo y comprobar su CRC",
    "usb.put": "Copiar un archivo",
    "usb.put.title": "Copia un archivo (hasta 700 KB) a la raíz del disco",
    "usb.rand": "Copiar 1 MB",
    "usb.rand.title": "Copia 1 MB de bytes aleatorios para medir la velocidad",
    "usb.toobig": "ese archivo pasa de 700 KB: usa «Copiar 1 MB» para medir la velocidad",
    "usb.types": "El teclado escribe:",
    "usb.layout.title": "Distribución del teclado del ordenador",
    "usb.clear": "Borrar",
    "usb.log": "Lo que hace el ordenador",
    // console and keys
    "console.h": "Consola",
    "keys": "Teclas: <kbd>T</kbd> arriba · <kbd>B</kbd> abajo · <kbd>←↑→↓</kbd> cruceta · clic en el cristal = dedo · <kbd>B</kbd>+<kbd>←</kbd> 1 s = reset · en una app, <kbd>B</kbd> mantenido vuelve a KodeOS",
    "log.uploading": (name, kb) => `[kode-sim] subiendo ${name} (${kb} KB)…`,
    "log.installed": (name) => `[kode-sim] ${name} instalada; KodeOS la arranca`,
    "log.install.failed": (name, why) => `[kode-sim] no se pudo instalar ${name}: ${why}`,
    "log.upload.failed": (name, why) => `[kode-sim] no se pudo subir ${name}: ${why}`,
    "log.mic.on": "[kode-sim] micrófono del Mac → micrófono del Kode Dot",
    "log.mic.off": (why) => `[kode-sim] sin micrófono (${why}); el Kode Dot oye solo su altavoz y su ruido de fondo`,
  },
  en: {
    "settings.title": "Battery, USB, SD card, motion, installing an app and the console",
    "settings.open": "⚙ Settings",
    "settings.close": "✕ Close",
    "more": "More",
    "less": "Less",
    "more.title": "Battery, USB, SD card, installing an app and the console",
    "reset.title": "Resets the chip, as the board's reset circuit does (bottom + left for 1 s)",
    "power.on": "Power on",
    "power.running": "On",
    "restart": "Restart",
    "state.connecting": "connecting…",
    "state.running": "on",
    "state.off": "off",
    "state.crashed": "crashed",
    "state.busy": "waiting",
    "banner.off": "Off. Press the bottom button to turn it on.",
    "banner.busy": "Every simulator is in use. This page tries again by itself every 15 s.",
    "banner.crashed": "The firmware has crashed.",
    "banner.offline": "No connection to kode-sim; retrying…",
    "sound.hint": "Click the device to turn the sound on",
    "sound.on": "Sound on",
    "mic.title": "What the Mac's microphone hears reaches the Kode Dot's. Off, the Kode Dot hears only its own speaker and its background noise",
    "mic.label": "Mac's microphone",
    "app.title": "An app built for the simulator: kode-sim build your-app prints its .bin",
    "app.label": "Install app (.bin)",
    "battery.h": "Battery",
    "battery.usb": "USB connected",
    "battery.sd": "SD inserted",
    "battery.note": "KodeOS follows it like a real battery: it works out the % from the voltage and moves it at most 2 points every 2 s.",
    "motion.h": "Motion (IMU and compass)",
    "motion.pitch": "Pitch",
    "motion.roll": "Roll",
    "motion.heading": "Heading",
    "motion.magnet": "Magnet",
    "motion.magnet.title": "Distance from the magnet to the back, in mm; at the end, no magnet",
    "motion.nomagnet": "no magnet",
    "motion.shake": "Shake",
    "motion.shake.title": "A 1.2 s shake",
    "motion.flat": "Flat",
    "motion.flat.title": "Flat, facing north, no magnet",
    "motion.compass": ["N", "NE", "E", "SE", "S", "SW", "W", "NW"],
    "usb.h": "Computer (USB)",
    "usb.stage.no_cable": "no cable",
    "usb.stage.waiting": "cable plugged in: nothing answers",
    "usb.stage.enumerating": "enumerating…",
    "usb.stage.mounted": "mounted",
    "usb.stage.keyboard": "keyboard",
    "usb.stage.ejected": "ejected: the cable can be unplugged now",
    "usb.stage.failed": "failed",
    "usb.plug": "Plug in cable",
    "usb.unplug": "Unplug cable",
    "usb.plug.title": "Plugs or unplugs the USB cable between the Kode Dot and this simulated computer",
    "usb.eject": "Eject",
    "usb.eject.title": "Eject the disk, as the operating system does before the cable comes out",
    "usb.device": "device",
    "usb.volume": (label, free, total, protectedDisk) =>
      `${label || "unnamed"} · ${free} free of ${total}${protectedDisk ? " · write-protected" : ""}`,
    "usb.file.title": "Read the file and check its CRC",
    "usb.put": "Copy a file",
    "usb.put.title": "Copies a file (up to 700 KB) to the disk's root",
    "usb.rand": "Copy 1 MB",
    "usb.rand.title": "Copies 1 MB of random bytes to measure the speed",
    "usb.toobig": "that file is over 700 KB: use \"Copy 1 MB\" to measure the speed",
    "usb.types": "The keyboard types:",
    "usb.layout.title": "The computer's keyboard layout",
    "usb.clear": "Clear",
    "usb.log": "What the computer does",
    "console.h": "Console",
    "keys": "Keys: <kbd>T</kbd> top · <kbd>B</kbd> bottom · <kbd>←↑→↓</kbd> D-pad · click the glass = finger · <kbd>B</kbd>+<kbd>←</kbd> 1 s = reset · in an app, holding <kbd>B</kbd> goes back to KodeOS",
    "log.uploading": (name, kb) => `[kode-sim] uploading ${name} (${kb} KB)…`,
    "log.installed": (name) => `[kode-sim] ${name} installed; KodeOS boots it`,
    "log.install.failed": (name, why) => `[kode-sim] could not install ${name}: ${why}`,
    "log.upload.failed": (name, why) => `[kode-sim] could not upload ${name}: ${why}`,
    "log.mic.on": "[kode-sim] Mac's microphone → Kode Dot's microphone",
    "log.mic.off": (why) => `[kode-sim] no microphone (${why}); the Kode Dot hears only its own speaker and its background noise`,
  },
};

let lang = "en";

function t(key, ...args) {
  const v = STRINGS[lang][key];
  return typeof v === "function" ? v(...args) : v;
}

// The remembered choice, else the browser's: Spanish when its first
// language is Spanish, English for every other.
function langWanted() {
  try {
    const kept = localStorage.getItem("kode-sim.lang");
    if (kept in STRINGS) return kept;
  } catch (_) { /* not kept */ }
  const first = (navigator.languages && navigator.languages[0]) || navigator.language || "";
  return /^es\b/i.test(first) ? "es" : "en";
}

// What the HTML names with data-i18n*, in the current language.
function translateStatic() {
  document.documentElement.lang = lang;
  for (const el of document.querySelectorAll("[data-i18n], [data-i18n-html], [data-i18n-title], [data-i18n-placeholder]")) {
    const d = el.dataset;
    if (d.i18n) el.textContent = t(d.i18n);
    if (d.i18nHtml) el.innerHTML = t(d.i18nHtml);
    if (d.i18nTitle) el.title = t(d.i18nTitle);
    if (d.i18nPlaceholder) el.placeholder = t(d.i18nPlaceholder);
  }
}
