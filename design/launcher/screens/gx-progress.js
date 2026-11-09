/* The update button's readout: what to draw for a stream of `gx-update-progress` events.
 *
 * GeneralsX @bugfix Claude 19/09/2026 The owner's button read "5.2 MB / 7.1 MB", then
 * "1.1 MB / 7.1 MB", then "1.0 MB / 4.3 MB". Two faults, both measured in his gxlaunch.log:
 * several updaters ran at once and all of them emitted to this one button, and each reported the
 * CURRENT FILE's bytes (7.1 MB is gxrt_d983ed3a.zip, 4.3 MB the engine zip), not the update's.
 * GeneralsXUpdate.exe now allows one run per install and reports the whole update; this file is
 * the belt to that brace - it never lets the fill go backwards within one total, and it only shows
 * a speed once there is enough of the transfer to state one.
 *
 * NO DOM HERE, so the arithmetic runs under plain node (scripts/test/launcher-progress-test.js).
 */
(function (root) {
  'use strict';

  var SAMPLE_MS = 500;       // one speed sample per half second of transfer
  var ALPHA = 0.3;           // exponential smoothing: a new sample moves the estimate 30%
  var STABLE_SAMPLES = 3;    // 1.5 s of transfer before a speed is shown at all
  // How far `done` may fall back and still be the same transfer: a quarter of the denominator, or
  // 256 KB when there is no denominator to take a quarter of. A relayed sample arriving out of
  // order is a poll behind - a few MB of a 66 MB update; a RESTARTED transfer drops most of the
  // way to zero. Nothing in between has been observed, which is why a threshold can tell them
  // apart at all. ponytail: a heuristic, and its ceiling is an abandon early enough that the
  // re-run's drop is under a quarter - the bar then pins until the re-run passes it. The exact
  // answer is a transfer generation in the updater's `progress:` line, which costs a change to
  // its stdout contract, gxio::release::parse_updater_line and both callers.
  var BACK_SLACK = 262144;   // 256 KB

  function mb(bytes) { return (bytes / 1048576).toFixed(1); }

  function createMeter() {
    var total = -1, done = 0;
    var sampleT = null, sampleDone = 0, rate = null, samples = 0;

    // (done, total, nowMs) -> { fraction: 0..1 or null when indeterminate, text }
    function update(d, t, now) {
      d = Math.max(0, +d || 0);
      t = Math.max(0, +t || 0);
      if (t !== total || done - d > Math.max(BACK_SLACK, t / 4)) {
        // A different denominator is a different transfer (or the first real one after an
        // attached run's "0 0"): start over rather than mixing the two.
        //
        // GeneralsX @bugfix Claude 20/09/2026 AND SO IS A BIG STEP BACKWARDS AT THE SAME TOTAL.
        // The holder abandons at 60 of 66 MB, the attached run takes the lock and re-runs the same
        // plan from 0 with the same denominator - so `Math.max` alone pinned the bar at 60 MB for
        // the whole second transfer, and pinned `done` meant the speed read 0 and was hidden too.
        // A frozen bar is the exact complaint this file was written to answer.
        total = t; done = d; sampleT = now; sampleDone = d; rate = null; samples = 0;
      } else {
        done = Math.max(done, d);          // monotonic within one total
      }

      if (now - sampleT >= SAMPLE_MS) {
        var r = (done - sampleDone) * 1000 / (now - sampleT);
        rate = rate === null ? r : rate + ALPHA * (r - rate);
        samples++;
        sampleT = now; sampleDone = done;
      }

      if (total <= 0) {
        return { fraction: null, text: done > 0 ? mb(done) + ' MB' : 'PREPARING…' };
      }
      var shown = Math.min(done, total);
      var text = mb(shown) + ' / ' + mb(total) + ' MB';
      if (samples >= STABLE_SAMPLES && shown < total && rate > 0) {
        text += '  ·  ' + mb(rate) + ' MB/s';
      }
      return { fraction: shown / total, text: text };
    }

    return { update: update };
  }

  var api = { createMeter: createMeter };
  if (typeof module !== 'undefined' && module.exports) { module.exports = api; }
  else { root.GxProgress = api; }
})(this);
