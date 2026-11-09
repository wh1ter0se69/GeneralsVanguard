// A screen is shown once, in its final state (owner, 27/10/2026, Windows: "At first opening the launcher takes me to [1]
// then shows this [2]. I do not want the things to load incorrectly then load correctly or snap frames").
//
// WHY A SCREEN SNAPPED. Every launcher screen is its own document, and its markup is the artboard: the default tab
// selected, sample numbers blanked to an em dash, the hide-key boxes "…", the stock pictures. What the player saved
// arrives only through `invoke` (Tauri IPC), which is asynchronous on every platform and answers after the document
// has painted. On Windows the host runs those commands one after another on its main thread, and the HUD screen's
// hudkeys_load reads the game install first, so the default state stayed up long enough to screenshot - then the
// saved state, the title bar's buttons and the live preview all landed together a moment later.
//
// THE HOST HAS NO SYNCHRONOUS WAY TO HAND A PAGE ITS STATE (no sync IPC in Tauri v2; the initialization script is
// fixed when the window is built), so the state cannot be in the first frame. The page is therefore held instead:
// loaded in <head>, before any markup exists, this hides the whole body until everything the screen registered with
// gxSettle.wait() has answered, its fonts are loaded and its pictures are decoded - then shows it in one frame.
// Nothing is ever shown that is about to change. While held, transitions are off, so state applied in the dark does
// not animate into view, and the run's one-time entrance (.gx-rise) waits to play until the page is seen.
//
// A HIDDEN PAGE CANNOT TAKE FOCUS (visibility:hidden ignores .focus()): gxSettle.whenShown(fn) runs fn after the reveal.
//
// FUSES. A load that never answers must not leave the window blank: the page is shown after FUSE_MS whatever is still
// out (each screen already paints an honest em dash and a sentence for a load that failed). Pictures get at most
// PICTURE_MS after the loads have answered: a slow disk shows the page with a picture still coming rather than nothing.
//
// STANDALONE (a browser with no window.__TAURI__ - the design-review pages and the packaging gate) nothing is held and
// the artboard shows exactly as designed.
(function () {
  'use strict';
  var FUSE_MS = 3000;
  var PICTURE_MS = 800;
  var root = document.documentElement;
  var waits = [];
  var pics = [];                       // gxSettle.picture() preloads: under the same PICTURE_MS cap as <img>s
  var later = [];
  var held = false;
  var shown = false;
  var leaving = false;
  var fuse = 0;

  function noop() {}
  function reveal(fromFuse) {
    if (shown) { return; }
    // A page on its way to another is never revealed by its loads settling - only the fuse shows it, should the
    // navigation never happen (Codex lfp-r7: a later answer must not reveal Home in the frame before the Update page).
    if (leaving && fromFuse !== true) { return; }
    shown = true;
    clearTimeout(fuse);
    if (held) {
      // Settle the styles the loads wrote while transitions were off, THEN give transitions back: removing the class
      // in the same style pass as a state change would animate that change in front of the player.
      void getComputedStyle(root).visibility;
      if (document.body) { void document.body.offsetWidth; }
      root.classList.remove('gx-settling');
      if (window.gxMarkEntered) { window.gxMarkEntered(); }
    }
    var run = later.splice(0);
    run.forEach(function (fn) { try { fn(); } catch (e) { console.warn('gxSettle.whenShown:', e); } });
  }

  function decoded(i) { return i.naturalWidth && i.decode ? i.decode().then(noop, noop) : undefined; }
  function preload(url) {
    var i = new Image();
    i.decoding = 'async';
    var p = new Promise(function (ok) { i.onload = ok; i.onerror = ok; }).then(function () { return decoded(i); });
    i.src = url;
    return p;
  }
  // Every <img> in the document that is still loading, and decoded once loaded.
  function images() {
    var imgs = Array.prototype.slice.call(document.images || []);
    return Promise.all(imgs.map(function (i) {
      var loaded = i.complete ? Promise.resolve() : new Promise(function (ok) {
        i.addEventListener('load', ok, { once: true });
        i.addEventListener('error', ok, { once: true });
      });
      return loaded.then(function () { return decoded(i); });
    }));
  }
  // Every picture drawn as a CSS background by an element that is laid out (held pages are laid out, only hidden):
  // the Keybinds cameos and the Legacy HUD preview's ground are backgrounds, which document.images does not list.
  function backgrounds() {
    var seen = {}, urls = [];
    var all = document.body ? document.body.getElementsByTagName('*') : [];
    for (var n = 0; n < all.length; n++) {
      var el = all[n];
      if (!el.getClientRects().length) { continue; }
      var bg = getComputedStyle(el).backgroundImage;
      if (!bg || bg === 'none') { continue; }
      bg.replace(/url\(\s*(['"]?)(.*?)\1\s*\)/g, function (m, q, u) {
        if (u && !seen[u]) { seen[u] = true; urls.push(u); }
        return m;
      });
    }
    return Promise.all(urls.map(preload));
  }
  function pictures() { return Promise.all([images(), backgrounds()].concat(pics.splice(0))); }
  function capped(p, ms) {
    return Promise.race([p, new Promise(function (ok) { setTimeout(ok, ms); })]);
  }

  window.gxSettle = {
    /// Hold the page until `p` settles (resolved or rejected). Returns `p`. A wait added while others are still out
    /// is waited for too; one added after the reveal changes nothing.
    wait: function (p) {
      if (!shown && p && typeof p.then === 'function') { waits.push(p); }
      return p;
    },
    /// Run `fn` once the page is visible: at once when it already is.
    whenShown: function (fn) { if (held && !shown) { later.push(fn); } else { fn(); } },
    /// Preload a picture the page draws as a CSS background, so the reveal does not show the box before the picture.
    picture: function (url) {
      if (shown || !url) { return; }
      pics.push(preload(url));
    },
    held: function () { return held && !shown; }
  };

  var T = window.__TAURI__;
  if (!T || !T.core || !T.core.invoke) { shown = true; return; }

  held = true;
  root.classList.add('gx-settling');
  var css = document.createElement('style');
  css.setAttribute('data-gx-settle', '');
  // EVERY descendant, and !important: an element given `visibility: visible` - inline (the Keybinds assignment panel),
  // or by an animation - would otherwise show through a hidden body while the rest of the page is still held.
  css.textContent =
    'html.gx-settling body,html.gx-settling body *{visibility:hidden!important}' +
    'html.gx-settling *,html.gx-settling *::before,html.gx-settling *::after{transition:none!important}' +
    'html.gx-settling .gx-rise{animation-play-state:paused}';
  (document.head || root).appendChild(css);
  fuse = setTimeout(function () { reveal(true); }, FUSE_MS);
  // LEAVING is seen on the navigation itself: the Navigation API's navigate event (Chromium / WebView2), and
  // beforeunload, which a script navigation (location.replace / href) also dispatches before the page goes.
  // A fragment change or a download leaves the page where it is, so neither counts.
  // The fuse restarts then: a navigation that commits normally is never beaten by it, one that never happens still
  // shows the page.
  function leave() {
    if (leaving || shown) { return; }
    leaving = true;
    clearTimeout(fuse);
    fuse = setTimeout(function () { reveal(true); }, FUSE_MS);
  }
  if (window.navigation && typeof window.navigation.addEventListener === 'function') {
    window.navigation.addEventListener('navigate', function (e) { if (!e.hashChange && !e.downloadRequest) { leave(); } });
  }
  window.addEventListener('beforeunload', leave);

  function settle() {
    // Every inline script has run by DOMContentLoaded, so every load the page starts on open is registered.
    // A wait can register another (a chained load): drain until a round brings none.
    var round = waits.splice(0);
    if (round.length) {
      Promise.all(round.map(function (p) { return Promise.resolve(p).then(noop, noop); })).then(settle);
      return;
    }
    var fonts = document.fonts && document.fonts.ready ? document.fonts.ready.then(noop, noop) : Promise.resolve();
    capped(Promise.all([fonts, pictures()]), PICTURE_MS).then(function () {
      if (waits.length || pics.length) { settle(); return; }
      // Checked again in the frame itself: a load that answers between here and that frame can still register one
      // (Codex lfp-r4) - Setup's progress reply calling finishFetch() is such a case.
      requestAnimationFrame(function () {
        if (waits.length || pics.length) { settle(); return; }
        reveal();
      });
    });
  }
  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', settle);
  } else {
    settle();
  }
})();
