// GeneralsX launcher: runs BEFORE every page's own script (Tauri `initialization_script`, injected
// by gxlauncher/src/main.rs on every navigation). Not linked from any page, so a browser preview of a
// screen keeps its entrance exactly as designed.
//
// THE ENTRANCE PLAYS ONCE PER LAUNCHER RUN (owner, 05/10/2026: "when I open the launcher for a split
// second I see this"). Every screen is its own document, so each page change replayed the 520 ms
// staggered `.gx-rise` fade from opacity 0 - the half-faded Keybinds page in his screenshot. Moving
// between settings pages is a frequent action: it should be instant. sessionStorage is per window and
// clears when the launcher closes, which is exactly the scope wanted. A storage that throws plays the
// entrance as before (never a page stuck hidden).
(function () {
  var seen = false;
  try { seen = !!window.sessionStorage.getItem('gx-entered'); } catch (e) { return; }
  // The entrance counts as spent only once a page has really been SHOWN: MainWeb stays hidden while it
  // holds for a verdict and may hand over to another page unseen (it marks the run itself on release).
  window.gxMarkEntered = function () { try { window.sessionStorage.setItem('gx-entered', '1'); } catch (e) { /* plays again */ } };
  window.addEventListener('load', function () {
    if (!document.documentElement.classList.contains('gx-hold')) { window.gxMarkEntered(); }
  });
  if (!seen) { return; }
  var add = function () {
    var s = document.createElement('style');
    s.setAttribute('data-gx-boot', 'seen');
    s.textContent = '.gx-rise{animation:none!important}';
    (document.head || document.documentElement).appendChild(s);
  };
  if (document.documentElement) { add(); return; }
  // WebView2 can run this before <html> exists.
  new MutationObserver(function (_, o) {
    if (document.documentElement) { o.disconnect(); add(); }
  }).observe(document, { childList: true });
})();
