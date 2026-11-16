# The page

`index.html`, `sim.js`, `i18n.js` and `audio-worklet.js` are the Kode Dot in
the browser: the glass, the buttons, the LED, the haptic, the speaker, the microphone and
the console, talking to the bridge over `/ws`. `i18n.js` holds the page's
words in Spanish and English; a string is added to both tables, and named
in the HTML with `data-i18n` or asked for in `sim.js` with `t(key)`.

`kode-dot.png` and `kode-dot.json` are kode-sdk's own render of the device
and its geometry (`components/kui/sim/device/`, kode-sdk a5e1e25, GPL-3.0),
copied unchanged: the glass sits at (96, 100), 410x502, one picture pixel per
panel pixel.
