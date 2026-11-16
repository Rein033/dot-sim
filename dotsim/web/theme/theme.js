/* Dot Sim's look, in one place: shell.js and home.js take every name, colour and
 * line of fine print from here, and theme.css has the styling that goes with it.
 * Another look (a Kode one, say) is these three files: theme.js, theme.css and the
 * mascot in dash.js.
 *
 * Today's look is docs.kode.diy's: warm paper, near-black text, Outfit, yellow for
 * "here", blue for actions. Dash is Dot Sim's own mascot, not Kody.
 */
window.DOTSIM_THEME = {
  name: 'Dot Sim',
  mascot: 'Dash',
  colours: {
    bg: '#DED7CD',
    text: '#191919',
    card: 'rgba(255,255,255,.92)',
    line: '#C2BAB0',
    muted: '#6E6A64',
    yellow: '#FFAC00',     // active, "you are here"
    blue: '#028AC2',       // buttons
    dark: '#191919',       // the console
    onDark: '#E8E8E8',
  },
  font: {
    family: 'Outfit',
    css: 'https://fonts.googleapis.com/css2?family=Outfit:wght@400;500;600;700;800&display=swap',
  },
  icon: '/kode-dot.png',
  docs: '/docs',
  legal: {
    text: 'Unofficial. Not made, approved or supported by Kode.',
    source: 'https://github.com/Rein033/sim',
  },
};
