// Ctrl+S (Cmd+S on macOS) saves the settings page it is pressed on (owner, 24/09/2026).
//
// ONE FILE, EVERY PLATFORM. The launcher is the same HTML in WebView2 (Windows), WKWebView (macOS)
// and WebKitGTK (Linux), and none of them binds Ctrl/Cmd+S to anything the launcher wants: Tauri's
// default macOS menu carries no Cmd+S item, so the key reaches the page on all three. Either
// modifier is accepted everywhere rather than guessing the platform - this tree never reads the
// user agent (see gx-chrome-wired) - and Win+S never reaches a webview anyway.
//
// WINDOW, CAPTURE PHASE, stopImmediatePropagation. The Keybinds and HUD screens arm a key capture
// with a document capture-phase listener; a window capture listener runs before it, so Ctrl+S
// saves instead of being bound as the letter S (Keybinds) or refused as a chord (HUD).
//
// The focused element is blurred first: a number box being typed into commits on blur, so the
// digits on screen are part of what gets saved.
(function () {
  function isSaveKey(e) {
    return (e.ctrlKey || e.metaKey) && !e.altKey && !e.shiftKey &&
           String(e.key || '').toLowerCase() === 's';
  }

  /// `save(e)` is the page's own save. Called once per press; auto-repeat is swallowed. It gets the keydown, so a page
  /// that is capturing a key chord (Keybinds > GENERAL) can take Ctrl+S as the chord instead of saving.
  window.gxSaveKey = function (save) {
    window.addEventListener('keydown', function (e) {
      if (!isSaveKey(e)) { return; }
      e.preventDefault();
      e.stopImmediatePropagation();
      if (e.repeat) { return; }
      var a = document.activeElement;
      if (a && a !== document.body && a.blur) { a.blur(); }
      save(e);
    }, true);
  };

  /// AUTOSAVE (owner, 26/09/2026: every setting saves on change, no Save button, no going back to
  /// make it stick). `flush()` is the page's own writer: it sends everything still unsaved, resolves
  /// once that has landed, and rejects - having already shown why with `say` - when a write is
  /// refused. The page keeps a refused change and calls `changed()` again on its next edit.
  ///
  /// DEBOUNCED: a write goes out AUTOSAVE_MS after the last change, so a run of spin-button clicks
  /// is one write, not one per click. `now()` skips the wait (Ctrl+S, a Restore button).
  ///
  /// LEAVING WRITES FIRST. Every route off the page calls window.gxLeave, which flushes before it
  /// navigates - a change still inside its debounce would otherwise be dropped by the navigation.
  /// A refused write keeps the page, with the reason on screen and one more sentence saying the
  /// next attempt leaves without it. That next attempt retries first, and leaves either way: the
  /// player has been told, and a settings page that cannot be left while Zero Hour runs is worse.
  /// "Next" means READ_MS or more after the warning: sooner is the same attempt - a double click
  /// on BACK - and stays, or a double click would discard what the first click just warned about.
  /// CLOSING THE WINDOW WAITS TOO (Codex t3-autosave-r1 P2). The page tells Rust while it holds an
  /// unwritten change (window_save_pending); a close request then asks the page first
  /// (gx-close-requested), which runs the same leave guard and answers (window_close_answer) - see
  /// cmd_window.rs, which also closes anyway if no answer comes. `pagehide` is left for what no
  /// close request precedes (Cmd+Q on macOS): it sends a waiting write, best effort.
  var AUTOSAVE_MS = 400;
  var READ_MS = 600;
  window.gxLeaveReadMs = READ_MS;          // the Keybinds page's own leave guard uses the same pause
  window.gxAutosave = function (flush, say) {
    var timer = null, warnedAt = 0, leaving = null, marked = false;
    var T = window.__TAURI__, invoke = T && T.core && T.core.invoke;
    function mark(pending) {
      if (!invoke || pending === marked) { return; }
      marked = pending;
      invoke('window_save_pending', { pending: pending }).catch(function () { /* an older Rust side */ });
    }
    function run() {
      clearTimeout(timer);
      timer = null;
      // A resolved flush means every change is on disk (each page's flush drains later ones).
      return flush().then(function (r) { warnedAt = 0; mark(false); return r; });
    }
    window.gxLeave = function () {
      if (leaving) { return leaving; }     // one attempt at a time
      leaving = run().then(function () { return true; }, function (e) {
        if (warnedAt && Date.now() - warnedAt >= READ_MS) { return true; }
        if (!warnedAt) { warnedAt = Date.now(); }
        // Said on every stay: the retry's own refusal has just replaced the line without it.
        say(window.gxSaveRefused(e, true), 'bad');
        return false;
      });
      leaving.then(function () { leaving = null; });
      return leaving;
    };
    window.addEventListener('pagehide', function () {
      if (timer) { run().catch(function () {}); }
    });
    if (invoke && T.event && T.event.listen) {
      T.event.listen('gx-close-requested', function (e) {
        window.gxLeave().then(function (ok) {
          invoke('window_close_answer', { ask: e.payload, close: ok }).catch(function () {});
        });
      });
    }
    return {
      changed: function () {
        mark(true);
        warnedAt = 0;                      // a newer change has not been warned about yet
        clearTimeout(timer);
        timer = setTimeout(function () { run().catch(function () { /* flush() said why */ }); }, AUTOSAVE_MS);
      },
      now: run
    };
  };

  /// A dim line above BACK, for pages with no save message slot of their own (General,
  /// Keybinds). General's own slot is unusable here: it refuses routine text while an error stands.
  var noteEl = null;
  window.gxSaveNote = function (text) {
    var save = document.querySelector('.gx-save');
    if (!noteEl && save && save.parentNode) {
      noteEl = document.createElement('div');
      noteEl.style.cssText = 'padding: 0 2px 10px 2px; font-size: var(--gx-fs-105); line-height: 1.5;' +
                             ' color: var(--gx-ink-dim);';
      save.parentNode.insertBefore(noteEl, save);
    }
    if (noteEl) { noteEl.textContent = text; }
  };

  /// The sentence for a refused save, on a page that KEEPS the unsaved choices. It says what
  /// happens next, and what it says is the only thing that happens: a refused change is written
  /// again by the next change, by Ctrl+S, or on the way off the page - never on a timer by itself.
  /// `leaving` adds what the next attempt to leave does (gxAutosave's leave guard).
  /// "Zero Hour is running" is the backend's refusal (cmd_options.rs refuse_while_game_running).
  window.gxSaveRefused = function (err, leaving) {
    var text = String(err);
    var s = /Zero Hour is running/i.test(text)
      ? 'Not saved: Zero Hour is running, and it rewrites Options.ini when it exits. ' +
        'Your changes are still on this page. Close the game, then press Ctrl+S (⌘S) or BACK to save them.'
      : 'Not saved: ' + text + ' Your changes are still on this page; Ctrl+S (⌘S) or BACK tries again.';
    return leaving ? s + ' Leave again to discard them.' : s;
  };
})();
