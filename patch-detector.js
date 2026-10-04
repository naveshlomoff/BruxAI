/*
 * BruxDetector -- live bruxism detection from the BroxMon Bridge's payload v2.
 *
 * The same rule as patch-bridge/tools/detect_v1.py, written to run as data arrives: every baseline
 * looks only at the past, and each ~91 ms sound frame is judged once the pressure and motion packets
 * covering it have arrived (DELAY_MS behind the newest data). Loaded by index.html
 * (window.BruxDetector) and by patch-bridge/tools/detector_replay.mjs, which replays a patch_capture
 * recording through it -- one implementation for both.
 *
 *   sound    - 250-2000 Hz level above its quiet floor (20th percentile of the last 30 s). A frame is
 *              grinding-like unless voice-like (<250 Hz rose >9 dB too: speech, coughs, teeth tapping;
 *              grinding stays under ~7) or broadband (250-2000 Hz rose <5 dB more than 2-4 kHz did:
 *              scratching or rubbing near the patch). Narrow 2.5-3 kHz tones (the phone's cue beeps)
 *              are masked.
 *   pressure - a rise over the median of the last 5 s, or a 1 s spread (clenching shows as a rise,
 *              grinding with the patch pressed as oscillation).
 *   motion   - accelerometer spread (0.5 s) over its quiet floor: jaw-sized, or the whole body
 *              (turning over, getting up).
 * Episodes merge hits less than 1.5 s apart and are graded when they end:
 *   likely   - sustained grinding-like sound together with a pressure event or jaw-sized motion
 *              (kind 'grind'), or -- temple -- a strong sustained clench (kind 'clench')
 *   maybe    - sound alone or a pressure event alone
 *   movement - most of the episode was gross body movement (flagged, not counted as bruxism)
 *
 * Two profiles (detect_v1.py has the measurements behind each number): 'temple', the default and the
 * chosen placement (30.09 + 04.10.2026 calibrations), and 'cheek', the first rule (28.09.2026). The
 * temple also asks grinding to be dense and lower-pitched (a yawn is neither) and lets nothing within
 * 5 s of a body movement be "likely" -- so its episodes are reported 5 s later.
 */
(function (root, factory) {
  if (typeof module === 'object' && module.exports) module.exports = factory();
  else root.BruxDetector = factory();
})(typeof self !== 'undefined' ? self : this, function () {
  'use strict';

  const BASE = {
    bandCount: 16,
    micLagMs: 224,          // the mic path trails the pressure/motion clock (measured from cue beeps)
    delayMs: 2500,          // judge a frame once its pressure/motion packets have surely arrived
    frameMs: 100,
    soundExcessDb: 8,
    voiceLowDb: 9,
    scratchMarginDb: 5,
    mergeMs: 1500,
    soundMinFrames: 6,
    soundMinMs: 1500,
    voiceVeto: 0.4,
    scratchVeto: 0.4,
    floorSpanMs: 30000,
    pressureSpanMs: 5000,
    packetSamples: 91,
    moveContextMs: 5000,
  };

  const PROFILES = {
    temple: {
      pressureRise: 100, pressureOsc: 50, pressMinFrames: 5,
      motionJaw: [1.8, 8], motionGross: 8,
      soundMinDensity: 0.45, soundMinTiltDb: -6,
      clenchRise: 120, clenchMinFrames: 8,
      moveContext: 15,
    },
    cheek: {
      pressureRise: 40, pressureOsc: 25, pressMinFrames: 3,
      motionJaw: [1.8, 4.5], motionGross: 8,
      soundMinDensity: null, soundMinTiltDb: null,
      clenchRise: null, clenchMinFrames: null,
      moveContext: null,
    },
  };

  const DEFAULTS = Object.assign({ profile: 'temple' }, BASE, PROFILES.temple);

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
    const profile = (options && options.profile) || DEFAULTS.profile;
    if (!PROFILES[profile]) throw new Error(`BruxDetector: unknown profile "${profile}"`);
    const cfg = Object.assign({}, BASE, PROFILES[profile], options || {}, { profile });
    const pending = [];                 // frames not judged yet: {t, powers}
    const recent = [];                  // judged frames kept for beep masking look-back: {t, beep}
    const history = [];                 // judged frames' {t, motion}, for the body-movement context
    const closing = [];                 // ended episodes waiting for moveContextMs of later frames
    const wins = {};
    for (const k of ['brux', 'low', 'high', 'lowmid', 'highmid', 'beepFloor', 'acc']) wins[k] = RollingWindow(cfg.floorSpanMs);
    const fsmWin = RollingWindow(cfg.pressureSpanMs);
    const fsm = PacketSeries(15000, cfg.packetSamples), acc = PacketSeries(15000, cfg.packetSamples);
    let episode = null;                 // open episode being accumulated
    let lastFrameT = -Infinity;
    const listeners = [];

    function reset() {
      pending.length = 0; recent.length = 0; history.length = 0; closing.length = 0;
      episode = null; lastFrameT = -Infinity;
      Object.values(wins).forEach(w => w.reset());
      fsmWin.reset(); fsm.reset(); acc.reset();
    }

    function isBeep(p) {
      // The phone's cue tones sit in 2500-2750 Hz and spill into 2750-3000 Hz; a beep is loud there
      // and not beside it. Coughs and scratches are loud at 2.5-3 kHz too, but broadband.
      const b = Math.max(toDb(p[10]), toDb(p[11]));
      const side = Math.max(toDb(p[9]), toDb(p[12]));
      const floor = wins.beepFloor.percentile(50, 20);
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

    // Each level above its own quiet floor (20th percentile of the last 30 s).
    function excess(name, value, t) {
      wins[name].push(t, value);
      return value - wins[name].percentile(20);
    }

    function judge(frame, beepNearby) {
      const p = frame.powers;
      wins.beepFloor.push(frame.t, toDb(p[10]));
      let brux = NaN, low = NaN, high = NaN, lowmid = NaN, highmid = NaN;
      if (!beepNearby) {
        brux = toDb(p[1] + p[2] + p[3] + p[4] + p[5] + p[6] + p[7]);   // 250-2000 Hz
        low = toDb(p[0]);                                              // <250 Hz
        high = toDb(p[8] + p[9] + p[12] + p[13] + p[14]);
        lowmid = toDb(p[1] + p[2] + p[3]);                             // 250-1000 Hz
        highmid = toDb(p[4] + p[5] + p[6] + p[7]);                     // 1000-2000 Hz
      }
      const bruxX = excess('brux', brux, frame.t), lowX = excess('low', low, frame.t), highX = excess('high', high, frame.t);
      const tilt = excess('lowmid', lowmid, frame.t) - excess('highmid', highmid, frame.t);
      const loud = bruxX > cfg.soundExcessDb;
      const voice = loud && lowX > cfg.voiceLowDb;
      const scratch = loud && !voice && (bruxX - highX) < cfg.scratchMarginDb;
      const soundHit = loud && !voice && !scratch;

      const [fsmMean, , fsmN] = fsm.stats(frame.t - 60, frame.t + 60);
      const fsmFrame = fsmN >= 3 ? fsmMean : NaN;
      fsmWin.push(frame.t, fsmFrame);
      const fsmDev = fsmFrame - fsmWin.percentile(50);
      const [, fsmSd, fsmSdN] = fsm.stats(frame.t - 500, frame.t + 500);
      const rise = fsmDev > cfg.pressureRise;
      const pressHit = rise || (fsmSdN >= 3 && fsmSd > cfg.pressureOsc);

      const [, accSd, accN] = acc.stats(frame.t - 250, frame.t + 250);
      const accFrame = accN >= 3 ? accSd : NaN;
      wins.acc.push(frame.t, accFrame);
      const motion = accFrame / Math.max(wins.acc.percentile(20), 1);
      const gross = motion > cfg.motionGross;
      const jaw = motion > cfg.motionJaw[0] && motion <= cfg.motionJaw[1];

      return { t: frame.t, soundHit, voice, scratch, pressHit, rise, fsmDev, gross, jaw, bruxX, motion, tilt };
    }

    // The episode has ended (no hit for mergeMs): grade it once moveContextMs of later frames are in.
    function endEpisode() {
      const frames = episode.frames.slice(0, episode.lastHit + 1);
      episode = null;
      closing.push({ frames, t0: frames[0].t, t1: frames[frames.length - 1].t });
    }

    function grade(e) {
      const { frames, t0, t1 } = e;
      const count = (k) => frames.reduce((n, f) => n + (f[k] ? 1 : 0), 0);
      const nSound = count('soundHit'), nPress = count('pressHit'), nVoice = count('voice'), nScratch = count('scratch');
      const loud = Math.max(nSound + nVoice + nScratch, 1);
      const duration = t1 - t0 + cfg.frameMs;
      let soundOk = nSound >= cfg.soundMinFrames && duration >= cfg.soundMinMs;
      if (nVoice / loud >= cfg.voiceVeto || nScratch / loud >= cfg.scratchVeto) soundOk = false;
      // Grinding fills the stretch it covers, and is loudest at 250-1000 Hz rather than above.
      const hits = [];
      frames.forEach((f, i) => { if (f.soundHit) hits.push(i); });
      const density = hits.length ? hits.length / (hits[hits.length - 1] - hits[0] + 1) : 0;
      const tilt = hits.length ? percentile(hits.map(i => frames[i].tilt), 50) : NaN;
      if (cfg.soundMinDensity != null && density < cfg.soundMinDensity) soundOk = false;
      if (cfg.soundMinTiltDb != null && !(tilt >= cfg.soundMinTiltDb)) soundOk = false;
      const pressOk = nPress >= cfg.pressMinFrames;
      if (!soundOk && !pressOk) return;
      const devs = frames.map(f => f.fsmDev).filter(Number.isFinite);
      const devMax = devs.length ? Math.max(...devs) : NaN;
      const clench = cfg.clenchRise != null && count('rise') >= cfg.clenchMinFrames && devMax >= cfg.clenchRise;
      const grossShare = count('gross') / frames.length, jawShare = count('jaw') / frames.length;
      let level, kind = soundOk ? 'grind' : 'clench';
      if (grossShare > 0.3) { level = 'movement'; kind = null; }
      else if (soundOk && (pressOk || jawShare > 0.3)) level = 'likely';
      else if (clench) { level = 'likely'; kind = 'clench'; }
      else level = 'maybe';
      if (level === 'likely' && cfg.moveContext != null) {
        // Settling into bed after turning over or getting up can sound like grinding.
        const near = history.filter(h => h.t >= t0 - cfg.moveContextMs && h.t <= t1 + cfg.moveContextMs && Number.isFinite(h.motion));
        if (near.length && Math.max(...near.map(h => h.motion)) > cfg.moveContext) level = 'maybe';
      }
      const peakDb = Math.max(...frames.map(f => (Number.isFinite(f.bruxX) ? f.bruxX : -Infinity)));
      const result = {
        t0, t1, level, kind, durationMs: duration,
        sound: soundOk, pressure: pressOk, jawMotion: jawShare > 0.3,
        frames: { sound: nSound, pressure: nPress, voice: nVoice, scratch: nScratch },
        peakDb: Number.isFinite(peakDb) ? Math.round(peakDb * 10) / 10 : null,
      };
      listeners.forEach(fn => fn(result));
    }

    // Grades ended episodes whose context is complete (all of them when `all`).
    function settle(latestT, all) {
      while (closing.length && (all || cfg.moveContext == null || latestT > closing[0].t1 + cfg.moveContextMs)) {
        grade(closing.shift());
      }
      const keepFrom = Math.min(episode ? episode.frames[0].t : Infinity, ...closing.map(c => c.t0), latestT) - cfg.moveContextMs;
      while (history.length && history[0].t < keepFrom) history.shift();
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
        history.push({ t: f.t, motion: f.motion });
        const hit = f.soundHit || f.pressHit;
        if (episode && f.t - episode.frames[episode.lastHit].t > cfg.mergeMs) endEpisode();
        if (hit) {
          if (!episode) episode = { frames: [], lastHit: 0 };
          episode.frames.push(f);
          episode.lastHit = episode.frames.length - 1;
        } else if (episode) {
          episode.frames.push(f);
        }
        settle(f.t, false);
      }
    }

    function flush() {
      process(Infinity);
      if (episode) endEpisode();
      settle(-Infinity, true);
      history.length = 0;
    }

    return {
      pushFrame, pushPressure, pushMotion, process, flush, reset,
      onEpisode(fn) { listeners.push(fn); },
      config: cfg,
    };
  }

  return { create, DEFAULTS, PROFILES };
});
