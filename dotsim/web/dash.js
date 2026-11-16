/* Dash: Dot Sim's own mascot, a small dog. Drawn on a canvas with plain shapes, so it
 * scales to any size. Original art for this site; not Kody, and not made by Kode.
 *
 *   Dash.draw(ctx, { x, y, s, t, mood, look, time })
 *     x, y   where his feet stand (bottom centre), in canvas pixels
 *     s      scale: 1 = about 200 px tall
 *     t      time in ms (performance.now())
 *     mood   'idle' | 'happy' | 'sleep' | 'clock' | 'notify' | 'charge' | 'wake'
 *     look   {x, y} in canvas pixels to look at, or null
 *     time   text for the clock card ('10:09'), for mood 'clock'
 *     note   text for the note, for mood 'notify'
 */
(function () {
  const C = {
    fur: '#E8A15A', furDark: '#C97E3A', cream: '#F8E7CC', ear: '#6E4426', earIn: '#8E5A34',
    ink: '#191919', white: '#FFFFFF', tongue: '#F06E8A', collar: '#028AC2', tag: '#FFAC00',
    blush: 'rgba(240,110,138,.35)', card: '#FFFFFF', line: '#C2BAB0',
  };

  function ell(ctx, x, y, rx, ry, rot, fill) {
    ctx.beginPath(); ctx.ellipse(x, y, Math.max(0.1, rx), Math.max(0.1, ry), rot || 0, 0, Math.PI * 2);
    ctx.fillStyle = fill; ctx.fill();
  }
  function rr(ctx, x, y, w, h, r) {
    ctx.beginPath(); ctx.moveTo(x + r, y); ctx.arcTo(x + w, y, x + w, y + h, r); ctx.arcTo(x + w, y + h, x, y + h, r);
    ctx.arcTo(x, y + h, x, y, r); ctx.arcTo(x, y, x + w, y, r); ctx.closePath();
  }
  // A blink every few seconds, from a fixed pseudo-random rhythm.
  function blink(t) {
    const p = t % 4300;
    if (p < 120) return 1 - Math.abs(p - 60) / 60;          // 0 open .. 1 shut
    const q = t % 9100;
    if (q > 2000 && q < 2240) return 1 - Math.abs(((q - 2000) % 120) - 60) / 60;  // a double blink
    return 0;
  }

  function draw(ctx, o) {
    const t = o.t || 0, mood = o.mood || 'idle', s = o.s || 1;
    const sleep = mood === 'sleep', happy = mood === 'happy' || mood === 'charge';
    const holding = mood === 'clock' || mood === 'notify';
    ctx.save();
    ctx.translate(o.x, o.y); ctx.scale(s, s);
    // breathing and bounce
    const br = Math.sin(t / (sleep ? 900 : 520)) * (sleep ? 2.2 : 1.4);
    const hop = happy ? Math.max(0, Math.sin(t / 140)) * 8 : 0;
    ctx.translate(0, -hop);

    // shadow
    ctx.save(); ctx.globalAlpha = 0.18; ell(ctx, 0, 2 + hop, 62 - hop * 1.5, 9, 0, C.ink); ctx.restore();

    // tail (behind), wagging
    const wag = sleep ? Math.sin(t / 1600) * 0.08 : Math.sin(t / (happy ? 70 : 210)) * (happy ? 0.55 : 0.3);
    ctx.save(); ctx.translate(40, -38); ctx.rotate(-0.9 + wag);
    ctx.lineCap = 'round'; ctx.strokeStyle = C.furDark; ctx.lineWidth = 15;
    ctx.beginPath(); ctx.moveTo(0, 0); ctx.quadraticCurveTo(26, -6, 34, -30); ctx.stroke();
    ctx.strokeStyle = C.cream; ctx.lineWidth = 9;
    ctx.beginPath(); ctx.moveTo(30, -20); ctx.lineTo(34, -30); ctx.stroke();
    ctx.restore();

    // body
    ell(ctx, 0, -50 + br * 0.3, 52, 50 + br, 0, C.fur);
    ell(ctx, 0, -40 + br * 0.3, 30, 36 + br * 0.6, 0, C.cream);
    // back paws
    ell(ctx, -38, -8, 20, 11, 0, C.furDark);
    ell(ctx, 38, -8, 20, 11, 0, C.furDark);

    // head, tilted when asleep or reading
    const tilt = sleep ? 0.22 : mood === 'notify' ? Math.sin(t / 900) * 0.06 : Math.sin(t / 2600) * 0.03;
    const hy = -118 + br * 0.6 + (sleep ? 10 : 0);
    ctx.save(); ctx.translate(0, hy); ctx.rotate(tilt);

    // ears: floppy, twitch now and then
    const tw = (t % 6200) < 260 ? Math.sin(((t % 6200) / 260) * Math.PI) * 0.35 : 0;
    const earDrop = sleep ? 0.25 : 0;
    for (const side of [-1, 1]) {
      ctx.save(); ctx.translate(side * 40, -26); ctx.rotate(side * (0.35 + earDrop) + (side > 0 ? -tw : 0));
      ell(ctx, side * 6, 26, 15, 34, 0, C.ear);
      ell(ctx, side * 5, 28, 8, 24, 0, C.earIn);
      ctx.restore();
    }
    ell(ctx, 0, 0, 52, 47, 0, C.fur);                // head
    ell(ctx, -20, -16, 15, 13, -0.3, C.furDark);     // the patch over his left eye
    ell(ctx, 0, 20, 30, 22, 0, C.cream);              // muzzle
    if (happy || mood === 'wake') { ell(ctx, -30, 16, 9, 6, 0, C.blush); ell(ctx, 30, 16, 9, 6, 0, C.blush); }

    // eyes
    let lx = 0, ly = 0;
    if (o.look && !sleep) {
      const gx = o.look.x - o.x, gy = o.look.y - (o.y + (hy - hop) * s);
      const d = Math.hypot(gx, gy) || 1;
      lx = gx / d * 4; ly = gy / d * 3.5;
    } else if (mood === 'notify') { lx = Math.sin(t / 380) * 3.5; ly = 3; }
    else if (mood === 'clock') { ly = 3.5; }
    const b = sleep ? 1 : (mood === 'wake' ? 0.6 + 0.4 * Math.max(0, Math.sin(t / 300)) : blink(t));
    for (const side of [-1, 1]) {
      const ex = side * 19, ey = -8;
      if (b > 0.85 || sleep) {
        ctx.strokeStyle = C.ink; ctx.lineWidth = 4; ctx.lineCap = 'round';
        ctx.beginPath();
        if (happy) ctx.arc(ex, ey + 4, 8, Math.PI * 1.15, Math.PI * 1.85);
        else ctx.arc(ex, ey - 2, 8, Math.PI * 0.15, Math.PI * 0.85);
        ctx.stroke();
      } else {
        ell(ctx, ex + lx, ey + ly, 7.5, 9.5 * (1 - b), 0, C.ink);
        ell(ctx, ex + lx + 2.5, ey + ly - 3 * (1 - b), 2.6, 2.6 * (1 - b), 0, C.white);
      }
    }
    // nose and mouth
    ctx.beginPath(); ctx.moveTo(-9, 8); ctx.quadraticCurveTo(0, 4, 9, 8); ctx.quadraticCurveTo(6, 17, 0, 18);
    ctx.quadraticCurveTo(-6, 17, -9, 8); ctx.fillStyle = C.ink; ctx.fill();
    ell(ctx, -3, 9, 2.6, 1.6, 0, 'rgba(255,255,255,.55)');
    ctx.strokeStyle = C.ink; ctx.lineWidth = 3; ctx.lineCap = 'round';
    ctx.beginPath(); ctx.moveTo(0, 18); ctx.lineTo(0, 24);
    ctx.moveTo(0, 24); ctx.quadraticCurveTo(-8, 31, -14, 25); ctx.moveTo(0, 24); ctx.quadraticCurveTo(8, 31, 14, 25);
    ctx.stroke();
    if (happy || mood === 'idle' && (t % 11000) > 8000) {
      const tl = happy ? 10 + Math.sin(t / 90) * 2 : 8;
      rr(ctx, -6, 25, 12, tl, 6); ctx.fillStyle = C.tongue; ctx.fill();
      ctx.strokeStyle = 'rgba(0,0,0,.18)'; ctx.lineWidth = 1.5; ctx.beginPath(); ctx.moveTo(0, 28); ctx.lineTo(0, 22 + tl); ctx.stroke();
    }
    ctx.restore(); // head

    // collar with a tag
    ctx.save(); ctx.translate(0, hy + 42);
    rr(ctx, -34, -5, 68, 11, 5); ctx.fillStyle = C.collar; ctx.fill();
    const sw = Math.sin(t / 300) * 0.15;
    ctx.translate(0, 6); ctx.rotate(sw);
    ell(ctx, 0, 8, 7.5, 7.5, 0, C.tag);
    ctx.fillStyle = C.ink; ctx.beginPath(); ctx.arc(0, 8, 2, 0, Math.PI * 2); ctx.fill();
    ctx.restore();

    // front paws, or what he holds up
    if (holding) {
      const cy = -50 + Math.sin(t / 700) * 1.5;
      ctx.save(); ctx.translate(0, cy); ctx.rotate(mood === 'notify' ? -0.06 : 0);
      ctx.shadowColor = 'rgba(0,0,0,.18)'; ctx.shadowBlur = 6; ctx.shadowOffsetY = 2;
      rr(ctx, -50, -21, 100, 42, 10); ctx.fillStyle = C.card; ctx.fill();
      ctx.shadowColor = 'transparent';
      ctx.strokeStyle = C.line; ctx.lineWidth = 2; ctx.stroke();
      ctx.fillStyle = C.ink; ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
      if (mood === 'clock') {
        ctx.font = '700 25px Outfit, system-ui, sans-serif';
        ctx.fillText(o.time || '', 0, 2);
      } else {
        ctx.fillStyle = C.tag; ctx.beginPath(); ctx.arc(-30, -10, 5, 0, Math.PI * 2); ctx.fill();
        ctx.fillStyle = '#B9B2A8';
        for (let i = 0; i < 3; i++) { rr(ctx, -20, -11 + i * 10, i === 2 ? 34 : 54, 5, 2.5); ctx.fill(); }
        if (o.note) {
          ctx.fillStyle = C.ink; ctx.font = '600 11px Outfit, system-ui, sans-serif';
          ctx.fillText(String(o.note).slice(0, 18), 0, 31);
        }
      }
      ctx.restore();
      ell(ctx, -40, cy + 14, 13, 11, 0, C.fur);       // paws on the card
      ell(ctx, 40, cy + 14, 13, 11, 0, C.fur);
      ell(ctx, -40, cy + 18, 8, 5, 0, C.cream);
      ell(ctx, 40, cy + 18, 8, 5, 0, C.cream);
    } else {
      ell(ctx, -17, -9, 14, 10, 0, C.fur);
      ell(ctx, 17, -9, 14, 10, 0, C.fur);
      ell(ctx, -17, -6, 8, 5, 0, C.cream);
      ell(ctx, 17, -6, 8, 5, 0, C.cream);
    }

    // extras: Zzz, the charge bolt
    if (sleep) {
      ctx.fillStyle = C.ink; ctx.textAlign = 'center';
      for (let i = 0; i < 3; i++) {
        const k = ((t / 1400) + i / 3) % 1;
        ctx.globalAlpha = Math.sin(k * Math.PI);
        ctx.font = '700 ' + (12 + k * 14) + 'px Outfit, system-ui, sans-serif';
        ctx.fillText('z', 52 + k * 26, hy - 40 - k * 60);
      }
      ctx.globalAlpha = 1;
    }
    if (mood === 'charge') {
      ctx.save(); ctx.translate(0, hy - 78 + Math.sin(t / 250) * 3);
      ctx.beginPath(); ctx.moveTo(4, -18); ctx.lineTo(-10, 3); ctx.lineTo(0, 3); ctx.lineTo(-4, 18);
      ctx.lineTo(10, -3); ctx.lineTo(0, -3); ctx.closePath();
      ctx.fillStyle = C.tag; ctx.fill(); ctx.strokeStyle = C.ink; ctx.lineWidth = 2; ctx.stroke();
      ctx.restore();
    }
    ctx.restore();
  }

  window.Dash = { draw, colors: C };
})();
