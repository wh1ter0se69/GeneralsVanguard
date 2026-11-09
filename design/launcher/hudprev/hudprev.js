/* The HUD page's GeneralsX UI preview: real game pixels, redrawn by the page's own controls.
 *
 * WHAT IT IS. hudprev/manifest.js (cut by scripts/design/hud-layers.py from GX_SHOT captures of one match) holds a base
 * picture, one layer per SpectatorShow* key, the plates ("chrome") that belong to no single key, the Tab scoreboard's
 * layers, and the two Hide-dead pictures. This file stacks them and shows or hides each from the controls on the page:
 *   - a switch  (data-spshow="Cash" ...)   -> that key's layer
 *   - Opacity   (#gx-spopacity, 25..100)    -> every layer's lo/hi pair, see below
 *   - Hide dead (#gx-sphidedead)            -> the picture with the dead player removed, over the bar and the board
 *   - XP (on the board)                       -> which of two board layouts: the column present or not
 *   - the view  (#gx-sppicbar: bar | board | play) -> whether the Tab scoreboard layers are drawn, or the Playing scene
 *   - Playing: the economy strip (#gx-plstrip) is its layer; Tab while playing (#gx-pltab) is a key, not a picture, so
 *     switched off it puts a note on the picture saying Tab keeps its old job
 * OPACITY. A layer is two images, "lo" (panels at 25 %) and "hi" (at 100 %). Every pixel is linear in the panel opacity
 * (a plate pixel is a*P + (1-a)*B, text is constant), so lo under hi with hi at (opacity-25)/75 is the game's pixel.
 * NOT CLAIMED: switches interact only through the pixels they own, so a plate shared by two keys keeps drawing while
 * either is on, and Hide dead is exact only with everything else on (its pictures are one piece each).
 * It reads the controls from the DOM and watches them, so it works in the launcher and in the static design page alike.
 */
(function () {
  'use strict';
  var M = window.GX_HUDPREV;
  var lastOpacity = 50;     // the last value the box held that was a value (typing '100' passes through 1 and 10)
  var DIFFK = ['XP', 'EventToast', 'FollowPanel', 'Minimap'];
  var inst = [];                       // every mounted stage (the page's own and the large view's)
  var wired = false;

  function $(id) { return document.getElementById(id); }
  function img(src, cls) {
    var i = new Image();
    i.decoding = 'async'; i.alt = ''; i.draggable = false;
    if (cls) { i.className = cls; }
    i.src = '../hudprev/' + src;
    return i;
  }
  function place(el, r) {
    el.style.left = (r.x / M.w * 100) + '%'; el.style.top = (r.y / M.h * 100) + '%';
    el.style.width = (r.w / M.w * 100) + '%'; el.style.height = (r.h / M.h * 100) + '%';
  }
  // A layer piece: lo and hi, stacked, both exactly where the pixels were in the 1920x1080 capture.
  function piece(r, tag) {
    var p = document.createElement('span');
    p.className = 'pvp'; p.setAttribute('aria-hidden', 'true');
    place(p, r);
    p.appendChild(img(r.lo, 'lo')); p.appendChild(img(r.hi, 'hi'));
    return p;
  }
  function group(rows, cls) {
    var g = document.createElement('span');
    g.className = 'pvl ' + (cls || '');
    (rows || []).forEach(function (r) { g.appendChild(piece(r)); });
    return g;
  }

  // ---- the top bar -------------------------------------------------------------------------------------------------
  // The game lays each card out by rule (GXSpectatorHud.inl drawBar / drawCard, 1080p): switching a field off moves the
  // others, so the bar is rebuilt rather than layered. Layouts (clock on/off x dead hidden) are real empty bars; each card
  // is a few sprites from one atlas, put where the game's formulas put them.
  var SP = ['name', 'pN', 'pP', 'Cash', 'Power', 'Income', 'UnitsBuildings', 'tag', 'time'];
  function pct(el, x, y, w, h) {
    el.style.left = (x / M.w * 100) + '%'; el.style.top = (y / M.h * 100) + '%';
    el.style.width = (w / M.w * 100) + '%'; el.style.height = (h / M.h * 100) + '%';
  }
  function slotRect(B, k) {
    if (k === 'name') { return [B.name[2], B.name[3]]; }
    if (k === 'pN') { return [B.pzN, B.pzN]; }
    if (k === 'pP') { return [B.pzP, B.pzP]; }
    if (k === 'tag') { return [B.tag[1], B.tag[2]]; }
    if (k === 'time') { return [B.time[1], B.time[2]]; }
    for (var i = 0; i < B.lines.length; i++) { if (B.lines[i][0] === k) { return [B.lines[i][2], B.lines[i][3]]; } }
    return [0, 0];
  }
  function buildBar(B, parent) {
    var bar = { B: B, lay: {}, cards: [] };
    var wrap = document.createElement('span');
    wrap.className = 'pvbar'; parent.appendChild(wrap);
    Object.keys(B.layouts || {}).forEach(function (L) { bar.lay[L] = group(B.layouts[L], 'lay-' + L); wrap.appendChild(bar.lay[L]); });
    var at = M.bar.atlas;
    B.cards.forEach(function (c) {
      var card = { c: c, sp: {} };
      SP.forEach(function (k) {
        var sz = slotRect(B, k);
        var sp = document.createElement('span');
        sp.className = 'pvsp off'; sp.setAttribute('aria-hidden', 'true');
        ['lo', 'hi'].forEach(function (tag) {
          var im = img(c[tag], tag);
          im.style.width = (at.w / sz[0] * 100) + '%'; im.style.height = (at.h / sz[1] * 100) + '%';
          im.style.left = '0'; im.style.top = (-at.slots[k] / sz[1] * 100) + '%';
          sp.appendChild(im);
        });
        sp.__w = sz[0]; sp.__h = sz[1];
        card.sp[k] = sp; wrap.appendChild(sp);
      });
      bar.cards.push(card);
    });
    return bar;
  }
  function placeBar(bar, s) {
    var B = bar.B, clock = !!s.show.ClockTotals, hd = !!s.hideDead;
    var key = (clock ? 'A' : 'B') + (hd ? '1' : '0');
    Object.keys(bar.lay).forEach(function (L) { tog(bar.lay[L], L === key); });
    var cenW = clock ? 300 : 16, barW = 2 * B.side + cenW, barX = (M.w - barW) / 2;
    var N = !!s.show.Names, P = !!s.show.Portraits;
    var order = ['Cash', 'Power', 'Income', 'UnitsBuildings'];
    bar.cards.forEach(function (card, i) {
      var x = i < 6 ? barX + B.side - 4 - (6 - i) * B.cw : barX + B.side + cenW + 4 + (i - 6) * B.cw;
      var y = B.cardY, ty0 = y + 5;
      var show = {};
      var put = function (k, px, py) { var e = card.sp[k]; show[k] = true; pct(e, px, py, e.__w, e.__h); };
      if (!(card.c.dead && hd)) {
        var pz = N ? B.pzN : B.pzP, tx = x + 10;
        if (P) {
          put(N ? 'pN' : 'pP', x + 6, N ? ty0 + B.NR : y + (B.cardH - pz) / 2);
          tx = x + 6 + pz + 6;
        }
        if (N) { put('name', x + 10, ty0); }
        var ty = ty0 + (N ? B.NR : 0);
        if (card.c.dead) {
          put('tag', tx, ty + 2);
          if (s.show.StatusTime) { put('time', tx, ty + 20); }
        } else {
          B.lines.forEach(function (ln) {
            if (s.show[ln[0]]) { put(ln[0], tx, ty); ty += ln[3]; }
          });
        }
      }
      SP.forEach(function (k) { tog(card.sp[k], !!show[k]); });
    });
  }

  function scene(cls, base) {
    var sc = document.createElement('div');
    sc.className = 'pvscene ' + cls;
    sc.appendChild(img(base, 'pvbase'));
    return sc;
  }
  function build(host) {
    var st = document.createElement('div');
    st.className = 'pvstage';
    st.setAttribute('role', 'img');
    var o = { host: st, g: {}, chrome: null };
    var sp = scene('spec', M.base); o.spec = sp;
    if (M.bar) { o.bar = buildBar(M.bar, sp); }
    o.chrome = group(M.chrome, 'chrome'); sp.appendChild(o.chrome);
    Object.keys(M.layers || {}).forEach(function (k) { o.g[k] = group(M.layers[k], 'k-' + k); sp.appendChild(o.g[k]); });
    if (M.board) {
      o.bchrome = group(M.board.chrome, 'bchrome'); sp.appendChild(o.bchrome);
      if (M.board.chromeNoXp) { o.bchromeX = group(M.board.chromeNoXp, 'bchromex'); sp.appendChild(o.bchromeX); }
      o.bg = {};
      Object.keys(M.board.layers || {}).forEach(function (k) { o.bg[k] = group(M.board.layers[k], 'bk-' + k); sp.appendChild(o.bg[k]); });
    }
    if (M.hidedead) {   // the picture with the dead player removed, drawn over everything while Hide dead is on
      if (M.hidedead.bar) { o.hdBar = group(M.hidedead.bar, 'hdbar'); sp.appendChild(o.hdBar); }
      o.hdBoard = group(M.hidedead.board, 'hdboard'); sp.appendChild(o.hdBoard);
    }
    st.appendChild(sp);
    if (M.play) {
      var pl = scene('play', M.play.base); o.play = pl;
      o.pchrome = group(M.play.chrome, 'pchrome'); pl.appendChild(o.pchrome);
      o.pg = {};
      Object.keys(M.play.layers || {}).forEach(function (k) { o.pg[k] = group(M.play.layers[k], 'pk-' + k); pl.appendChild(o.pg[k]); });
      st.appendChild(pl);
    }
    var note = document.createElement('span');
    note.className = 'pvnote'; note.hidden = true;
    st.appendChild(note); o.note = note;
    host.appendChild(st);
    o.baseImg = st.querySelector('.pvbase');
    inst.push(o);
    return o;
  }

  // What the page's controls say right now.
  function state() {
    var s = { show: {}, any: false, anyDiff: false, hideDead: false, opacity: lastOpacity, view: 'bar', strip: true, ptab: true };
    Array.prototype.forEach.call(document.querySelectorAll('[data-spshow]'), function (e) {
      var on = e.getAttribute('aria-checked') === 'true';
      s.show[e.getAttribute('data-spshow')] = on;
      if (on && e.getAttribute('data-spshow') !== 'TabBoard') { s.any = true; if (DIFFK.indexOf(e.getAttribute('data-spshow')) >= 0) { s.anyDiff = true; } }
    });
    var hd = $('gx-sphidedead'); s.hideDead = !!hd && hd.getAttribute('aria-checked') === 'true';
    var op = $('gx-spopacity'); var n = op ? parseInt(op.textContent, 10) : NaN;
    // Typing '100' passes through '1' and '10': below the range while the box has focus is mid-keystroke, not a value.
    if (n === n && !(n < 25 && document.activeElement === op)) { s.opacity = lastOpacity = Math.max(25, Math.min(100, n)); }   // mid-typing text keeps the last good value
    var on = document.querySelector('#gx-sppicbar [data-pic].on');
    var v = on && on.getAttribute('data-pic');
    s.view = v === 'board' || v === 'play' ? v : 'bar';
    var ps = $('gx-plstrip'), pt = $('gx-pltab');
    s.strip = !ps || ps.getAttribute('aria-checked') === 'true';
    s.ptab = !pt || pt.getAttribute('aria-checked') === 'true';
    return s;
  }
  function tog(el, on) { if (el) { el.classList.toggle('off', !on); } }

  function paint(o, s) {
    var t = (s.opacity - 25) / 75;
    o.host.style.setProperty('--pvt', String(t));
    var playing = s.view === 'play' && !!o.play;
    tog(o.spec, !playing); tog(o.play, playing);
    if (o.bar) { placeBar(o.bar, s); }
    tog(o.chrome, o.bar ? s.anyDiff : s.any);
    Object.keys(o.g).forEach(function (k) { tog(o.g[k], !!s.show[k]); });
    var board = s.view === 'board' && !!s.show.TabBoard && !!M.board;
    tog(o.bchrome, board);
    tog(o.bchromeX, board && !s.show.XP);        // the XP column re-lays the board: this is what differs without it (status/time baked in)
    // (Status and time is a layer of the XP-column layout only: without the column it is baked into the patch.)
    if (o.bg) { Object.keys(o.bg).forEach(function (k) { tog(o.bg[k], board && !!s.show[k] && (!o.bchromeX || !!s.show.XP)); }); }
    tog(o.hdBar, s.hideDead && s.any); tog(o.hdBoard, board && s.hideDead);
    if (o.pg) { tog(o.pchrome, true); Object.keys(o.pg).forEach(function (k) { tog(o.pg[k], k === 'Strip' ? s.strip : true); }); }
    var msg = '';
    if (playing) { if (!s.ptab) { msg = 'Tab scoreboard is off while playing: Tab keeps its old job (Diplomacy).'; } }
    else if (s.view === 'board' && !s.show.TabBoard) { msg = 'Tab scoreboard is off: Tab does nothing in a match.'; }
    else if (!s.any) { msg = 'Everything is off: only the bar\u2019s empty frames are left.'; }
    o.note.textContent = msg; o.note.hidden = !msg;
    var shown = Object.keys(s.show).filter(function (k) { return s.show[k] && k !== 'TabBoard'; });
    o.host.setAttribute('aria-label', playing
      ? 'The match as you see it while playing, economy line ' + (s.strip ? 'shown' : 'hidden') + '. Panel opacity ' + s.opacity + ' percent' + (s.ptab ? '' : ', Tab keeps Diplomacy') + '.'
      : 'A spectated match as the game draws it with these switched on: ' +
      (shown.length ? shown.join(', ') : 'nothing') + '. Panel opacity ' + s.opacity + ' percent' +
      (s.view === 'board' ? (s.show.TabBoard ? ', with the Tab scoreboard open' : ', Tab scoreboard is off') : '') +
      (s.hideDead ? ', dead players hidden' : '') + '.');
  }

  var queued = false;
  function render() {
    if (queued) { return; }
    queued = true;
    requestAnimationFrame(function () { queued = false; var s = state(); inst.forEach(function (o) { paint(o, s); }); });
  }

  function watch() {
    if (wired) { return; }
    wired = true;
    var mo = new MutationObserver(render);
    ['gx-spectator', 'gx-player', 'gx-sppicbar'].forEach(function (id) {
      var e = $(id);
      if (e) { mo.observe(e, { subtree: true, attributes: true, attributeFilter: ['aria-checked', 'class'], childList: true, characterData: true }); }
    });
  }

  window.gxHudPreview = {
    // A manifest without the pieces the stage is built from is not a preview: the page keeps its stills.
    ready: !!(M && M.base && M.layers && M.chrome && M.bar && M.bar.layouts && M.bar.cards && M.bar.atlas),
    hasPlay: !!(M && M.play),
    mount: function (host) { if (!M) { return null; } var o = build(host); watch(); render(); return o; },
    unmount: function (o) { var i = inst.indexOf(o); if (i >= 0) { inst.splice(i, 1); } if (o && o.host.parentNode) { o.host.parentNode.removeChild(o.host); } },
    render: render
  };
})();
