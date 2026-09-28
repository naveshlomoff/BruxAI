/*
 * BruxDetector -- live bruxism detection from the BroxMon Bridge's payload v2.
 *
 * The same rule as patch-bridge/tools/detect_v1.py (tuned on the 28.09.2026 calibration), written to
 * run as data arrives: every baseline looks only at the past, and each ~91 ms sound frame is judged
 * once the pressure and motion packets covering it have arrived (DELAY_MS behind the newest data).
 * Loaded by index.html (window.BruxDetector) and by patch-bridge/tools/detector_replay.mjs, which
 * replays a patch_capture recording through it -- one implementation for both.
 *
 *   sound    - 250-2000 Hz level above its quiet floor (20th percentile of the last 30 s). A frame is
 *              grinding-like unless voice-like (<250 Hz rose >9 dB too: speech, coughs, teeth tapping;
 *              grinding stays under ~7) or broadband (250-2000 Hz rose <5 dB more than 2-4 kHz did:
 *              scratching or rubbing near the patch). Narrow 2.5-3 kHz tones (the phone's cue beeps)
 *              are masked.
 *   pressure - a rise over the median of the last 5 s, or a 1 s spread (grinding with the cheek
 *              pressed shows as oscillation, clenching as a rise).
 *   motion   - accelerometer spread (0.5 s) over its quiet floor: x1.8-4.5 is jaw-sized, over x8 is
 *              the whole body (turning over, getting up).
 * Episodes merge hits less than 1.5 s apart and are graded when they end:
 *   likely   - sustained grinding-like sound (>= 0.6 s of it over >= 1.5 s, not mostly voice or
 *              broadband) together with a pressure event or jaw-sized motion
 *   maybe    - that sound alone, or a pressure event alone
 *   movement - most of the episode was gross body movement (flagged, not counted as bruxism)
 */
(function (root, factory) {
  if (typeof module === 'object' && module.exports) module.exports = factory();
  else root.BruxDetector = factory();
})(typeof self !== 'undefined' ? self : this, function () {
  'use strict';

  const DEFAULTS = {
    bandCount: 16,
    micLagMs: 224,          // the mic path trails the pressure/motion clock (measured from cue beeps)
    delayMs: 2500,          // judge a frame once its pressure/motion packets have surely arrived
    frameMs: 100,
    soundExcessDb: 8,
    voiceLowDb: 9,
    scratchMarginDb: 5,
    pressureRise: 40,
    pressureOsc: 25,
    motionJaw: [1.8, 4.5],
    motionGross: 8,
    mergeMs: 1500,
    soundMinFrames: 6,
    soundMinMs: 1500,
    voiceVeto: 0.4,
    scratchVeto: 0.4,
    pressMinFrames: 3,
    floorSpanMs: 30000,
    pressureSpanMs: 5000,
    packetSamples: 91,
  };

  const toDb = (x) => 10 * Math.log10(x + 1e-9);

  // numpy.percentile's default (linear) on the finite values of `arr`.
  function percentile(arr, p) {
    const v = arr.filter(Number.isFinite).sort((a, b) => a - b);
    if (!v.length) return NaN;
    const pos = (v.length - 1) * p / 100;
    const lo = Math.floor(pos), hi = Math.ceil(pos);
    return v[lo] + (v[hi] - v[lo]) * (pos - lo);
  }

  // Keeps (t, value) pairs for `spanMs` and answers "percentile of the past span up to t".
  function RollingWindow(spanMs) {
    const ts = [], vs = [];
    return {
      push(t, v) {
        ts.push(t); vs.push(v);
        while (ts.length && ts[0] < t - spanMs) { ts.shift(); vs.shift(); }
      },
      percentile(p, minCount = 5) {
        const finite = vs.filter(Number.isFinite);
        return finite.length >= minCount ? percentile(finite, p) : NaN;
      },
      reset() { ts.length = 0; vs.length = 0; },
    };
  }

  // Samples that arrive in 91-sample packets, each timed back from its arrival at the bridge.
  function PacketSeries(keepMs, packetSamples) {
    const t = [], v = [];
    let lastArrival = null;
    const gaps = [];
    function typicalGap() {
      if (!gaps.length) return 1000;
      const s = gaps.slice().sort((a, b) => a - b);
      return s[Math.floor(s.length / 2)];
    }
    return {
      push(arrival, values) {
        const typical = typicalGap();
        let span = typical;
        if (lastArrival !== null) {
          const gap = arrival - lastArrival;
          if (gap > 300 && gap < 3000) { gaps.push(gap); if (gaps.length > 50) gaps.shift(); }
          if (gap > 0.6 * typical && gap < 1.6 * typical) span = gap;
        }
        lastArrival = arrival;
        const n = values.length, dt = span / packetSamples;
        for (let i = 0; i < n; i++) { t.push(arrival - (n - 1 - i) * dt); v.push(values[i]); }
        const cutoff = arrival - keepMs;
        let drop = 0;
        while (drop < t.length && t[drop] < cutoff) drop++;
        if (drop) { t.splice(0, drop); v.splice(0, drop); }
      },
      // [mean, std, count] of the samples within [from, to)
      stats(from, to) {
        let n = 0, sum = 0, sq = 0;
        for (let i = 0; i < t.length; i++) {
          if (t[i] >= from && t[i] < to) { n++; sum += v[i]; sq += v[i] * v[i]; }
        }
        if (!n) return [NaN, NaN, 0];
        const mean = sum / n;
        return [mean, Math.sqrt(Math.max(0, sq / n - mean * mean)), n];
      },
      reset() { t.length = 0; v.length = 0; gaps.length = 0; lastArrival = null; },
    };
  }

  function create(options) {
    const cfg = Object.assign({}, DEFAULTS, options || {});
    const pending = [];                 // frames not judged yet: {t, powers}
    const recent = [];                  // judged frames kept for beep masking look-back: {t, beep}
    const bruxWin = RollingWindow(cfg.floorSpanMs), lowWin = RollingWindow(cfg.floorSpanMs);
    const highWin = RollingWindow(cfg.floorSpanMs), beepFloorWin = RollingWindow(cfg.floorSpanMs);
    const accWin = RollingWindow(cfg.floorSpanMs), fsmWin = RollingWindow(cfg.pressureSpanMs);
    const fsm = PacketSeries(15000, cfg.packetSamples), acc = PacketSeries(15000, cfg.packetSamples);
    let episode = null;                 // open episode being accumulated
    let lastFrameT = -Infinity;
    const listeners = [];

    function reset() {
      pending.length = 0; recent.length = 0; episode = null; lastFrameT = -Infinity;
      [bruxWin, lowWin, highWin, beepFloorWin, accWin, fsmWin].forEach(w => w.reset());
      fsm.reset(); acc.reset();
    }

    function isBeep(p) {
      // The phone's cue tones sit in 2500-2750 Hz and spill into 2750-3000 Hz; a beep is loud there
      // and not beside it. Coughs and scratches are loud at 2.5-3 kHz too, but broadband.
      const b = Math.max(toDb(p[10]), toDb(p[11]));
      const side = Math.max(toDb(p[9]), toDb(p[12]));
      const floor = beepFloorWin.percentile(50, 20);
      return Number.isFinite(floor) && b > floor + 15 && b - side > 8;
    }

    // One sound frame, bridge clock (ms) of its last packet, 16 band powers in counts^2.
    function pushFrame(bridgeT, powers) {
      const t = bridgeT - cfg.micLagMs;
      if (t < lastFrameT - 5000) reset(); // bridge restarted: its clock went back
      lastFrameT = Math.max(lastFrameT, t);
      pending.push({ t, powers });
    }

    function pushPressure(arrival, values) { fsm.push(arrival, values); }
    function pushMotion(arrival, values) { acc.push(arrival, values); }

    function judge(frame, beepNearby) {
      const p = frame.powers;
      beepFloorWin.push(frame.t, toDb(p[10]));
      let brux = NaN, low = NaN, high = NaN;
      if (!beepNearby) {
        brux = toDb(p[1] + p[2] + p[3] + p[4] + p[5] + p[6] + p[7]);
        low = toDb(p[0]);
        high = toDb(p[8] + p[9] + p[12] + p[13] + p[14]);
      }
      bruxWin.push(frame.t, brux); lowWin.push(frame.t, low); highWin.push(frame.t, high);
      const bruxX = brux - bruxWin.percentile(20), lowX = low - lowWin.percentile(20), highX = high - highWin.percentile(20);
      const loud = bruxX > cfg.soundExcessDb;
      const voice = loud && lowX > cfg.voiceLowDb;
      const scratch = loud && !voice && (bruxX - highX) < cfg.scratchMarginDb;
      const soundHit = loud && !voice && !scratch;

      const [fsmMean, , fsmN] = fsm.stats(frame.t - 60, frame.t + 60);
      const fsmFrame = fsmN >= 3 ? fsmMean : NaN;
      fsmWin.push(frame.t, fsmFrame);
      const fsmDev = fsmFrame - fsmWin.percentile(50);
      const [, fsmSd, fsmSdN] = fsm.stats(frame.t - 500, frame.t + 500);
      const pressHit = fsmDev > cfg.pressureRise || (fsmSdN >= 3 && fsmSd > cfg.pressureOsc);

      const [, accSd, accN] = acc.stats(frame.t - 250, frame.t + 250);
      const accFrame = accN >= 3 ? accSd : NaN;
      accWin.push(frame.t, accFrame);
      const motion = accFrame / Math.max(accWin.percentile(20), 1);
      const gross = motion > cfg.motionGross;
      const jaw = motion > cfg.motionJaw[0] && motion <= cfg.motionJaw[1];

      return { t: frame.t, soundHit, voice, scratch, pressHit, gross, jaw, bruxX, motion };
    }

    function closeEpisode() {
      const e = episode;
      episode = null;
      const frames = e.frames.slice(0, e.lastHit + 1);
      const count = (k) => frames.reduce((n, f) => n + (f[k] ? 1 : 0), 0);
      const nSound = count('soundHit'), nPress = count('pressHit'), nVoice = count('voice'), nScratch = count('scratch');
      const loud = Math.max(nSound + nVoice + nScratch, 1);
      const t0 = frames[0].t, t1 = frames[frames.length - 1].t;
      const duration = t1 - t0 + cfg.frameMs;
      let soundOk = nSound >= cfg.soundMinFrames && duration >= cfg.soundMinMs;
      if (nVoice / loud >= cfg.voiceVeto || nScratch / loud >= cfg.scratchVeto) soundOk = false;
      const pressOk = nPress >= cfg.pressMinFrames;
      if (!soundOk && !pressOk) return;
      const grossShare = count('gross') / frames.length, jawShare = count('jaw') / frames.length;
      const level = grossShare > 0.3 ? 'movement'
        : soundOk && (pressOk || jawShare > 0.3) ? 'likely'
        : 'maybe';
      const peakDb = Math.max(...frames.map(f => (Number.isFinite(f.bruxX) ? f.bruxX : -Infinity)));
      const result = {
        t0, t1, level, durationMs: duration,
        sound: soundOk, pressure: pressOk, jawMotion: jawShare > 0.3,
        frames: { sound: nSound, pressure: nPress, voice: nVoice, scratch: nScratch },
        peakDb: Number.isFinite(peakDb) ? Math.round(peakDb * 10) / 10 : null,
      };
      listeners.forEach(fn => fn(result));
    }

    // Judges every frame older than DELAY_MS before `nowBridgeMs` (the newest flush's bridge clock).
    function process(nowBridgeMs) {
      pending.sort((a, b) => a.t - b.t);
      while (pending.length && pending[0].t <= nowBridgeMs - cfg.micLagMs - cfg.delayMs) {
        const frame = pending.shift();
        // Beep masking looks 150 ms back and 350 ms ahead, like the offline analysis.
        const around = recent.concat(pending).filter(f => f.t >= frame.t - 350 && f.t <= frame.t + 150);
        const beepNearby = around.some(f => (f.beep !== undefined ? f.beep : (f.beep = isBeep(f.powers))))
          || isBeep(frame.powers);
        frame.beep = isBeep(frame.powers);
        recent.push(frame);
        while (recent.length && recent[0].t < frame.t - 1000) recent.shift();
        const f = judge(frame, beepNearby);
        const hit = f.soundHit || f.pressHit;
        if (episode && f.t - episode.frames[episode.lastHit].t > cfg.mergeMs) closeEpisode();
        if (hit) {
          if (!episode) episode = { frames: [], lastHit: 0 };
          episode.frames.push(f);
          episode.lastHit = episode.frames.length - 1;
        } else if (episode) {
          episode.frames.push(f);
        }
      }
    }

    function flush() {
      process(Infinity);
      if (episode) closeEpisode();
    }

    return {
      pushFrame, pushPressure, pushMotion, process, flush, reset,
      onEpisode(fn) { listeners.push(fn); },
      config: cfg,
    };
  }

  return { create, DEFAULTS };
});
