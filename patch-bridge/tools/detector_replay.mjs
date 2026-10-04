#!/usr/bin/env node
// Replays a patch_capture.mjs recording through patch-detector.js (the file the app loads), flush by
// flush as the app receives them, and scores the episodes against the calibration steps -- the same
// table as detect_v1.py, so the live implementation can be checked against the reference.
//
//   node detector_replay.mjs <capture.jsonl>... [--profile temple|cheek] [--list]
//
// Several captures (a calibration finished on another day) are merged in time order. The bridge's
// clock and flush counter restart when it reboots; each clock epoch gets its own offset and every
// time is moved onto the PC clock, so the detector sees one steady timeline.

import fs from 'node:fs';
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const BruxDetector = require('../../patch-detector.js');

const args = process.argv.slice(2);
const profileAt = args.indexOf('--profile');
const profile = profileAt >= 0 ? args[profileAt + 1] : 'temple';
const files = args.filter((a, i) => !a.startsWith('--') && !(profileAt >= 0 && i === profileAt + 1));
if (!files.length) { console.error('usage: node detector_replay.mjs <capture.jsonl>... [--profile temple|cheek] [--list]'); process.exit(1); }
const PHONE_LATENCY_MS = 100;
const BAND_COUNT = 16, FRAME_BYTES = 4 + BAND_COUNT, PACKET = 91;

const lines = files.flatMap((f) => fs.readFileSync(f, 'utf8').split('\n').filter(Boolean).map((l) => JSON.parse(l)));
const flushes = lines.filter((l) => l.e === 'sample' && l.p.v === 2).sort((a, b) => a.r - b.r);
const calib = lines.filter((l) => l.e === 'calib').sort((a, b) => a.r - b.r);
let epoch = 0, lastQ = null;
for (const f of flushes) {
  if (lastQ !== null && f.p.q < lastQ) epoch++;
  lastQ = f.p.q;
  f.epoch = epoch;
}
const shifts = new Map();
for (const f of flushes) shifts.set(f.epoch, Math.min(shifts.get(f.epoch) ?? Infinity, f.r - f.p.t));
const offset = 0; // every time below is on the PC clock

const detector = BruxDetector.create({ profile });
const episodes = [];
detector.onEpisode((e) => episodes.push(e));

const u16 = (b64) => { const b = Buffer.from(b64 || '', 'base64'); return Array.from({ length: b.length >> 1 }, (_, i) => b.readUInt16LE(i * 2)); };
for (const { p, epoch } of flushes) {
  const shift = shifts.get(epoch);
  const bf = Buffer.from(p.bf || '', 'base64');
  for (let off = 0; off + FRAME_BYTES <= bf.length; off += FRAME_BYTES) {
    const powers = [];
    for (let b = 0; b < BAND_COUNT; b++) powers.push(Math.pow(10, (bf[off + 4 + b] - 40) / 20));
    detector.pushFrame(bf.readUInt32LE(off) + shift, powers);
  }
  for (const [key, times, push] of [['acc', p.at, detector.pushMotion], ['fsm', p.ft, detector.pushPressure]]) {
    const vals = u16(p[key]);
    if (!times || vals.length !== times.length * PACKET) continue;
    times.forEach((t, k) => push(t + shift, vals.slice(k * PACKET, (k + 1) * PACKET)));
  }
  detector.process(p.t + shift);
}
detector.flush();

// Calibration runs: start -> phases -> end.
const runs = [];
let cur = null;
for (const { r, p } of calib) {
  if (p.type === 'start') { cur = { step: p.step, start: r, phases: [] }; runs.push(cur); }
  else if (cur && p.step === cur.step && p.type === 'phase') cur.phases.push({ phase: p.phase, r });
  else if (cur && p.step === cur.step && (p.type === 'end' || p.type === 'abort')) { cur.end = r; cur.aborted = p.type === 'abort'; cur = null; }
}
for (const run of runs) run.phases.forEach((x, i) => { x.end = i + 1 < run.phases.length ? run.phases[i + 1].r : (run.end ?? x.r); });

const toBridge = (r) => r - offset - PHONE_LATENCY_MS;
console.log(`${episodes.length} episodes: ${['likely', 'maybe', 'movement'].map((l) => `${l} ${episodes.filter((e) => e.level === l).length}`).join(', ')}`);
console.log(`${'step'.padEnd(18)} ${'do phases caught (maybe / likely)'.padStart(34)} ${'false alarms in rest'.padStart(22)}   movement`);
for (const run of runs.filter((r) => !r.aborted && r.phases.length)) {
  const dos = run.phases.filter((x) => x.phase === 'do');
  let maybe = 0, likely = 0;
  for (const x of dos) {
    const lo = toBridge(x.r) + 300, hi = toBridge(x.end) + 800;
    const levels = episodes.filter((e) => e.t1 >= lo && e.t0 <= hi).map((e) => e.level);
    if (levels.includes('likely')) likely++; else if (levels.includes('maybe')) maybe++;
  }
  let restMs = 0, alarmMs = 0;
  for (const x of run.phases.filter((x) => ['rest', 'lead', 'tail'].includes(x.phase))) {
    const lo = toBridge(x.r) + 1500, hi = toBridge(x.end) - 200;
    if (hi <= lo) continue;
    restMs += hi - lo;
    for (const e of episodes) if (e.level !== 'movement') alarmMs += Math.max(0, Math.min(hi, e.t1 + 100) - Math.max(lo, e.t0));
  }
  const moves = episodes.filter((e) => e.level === 'movement' && e.t1 >= run.start - offset && e.t0 <= (run.end ?? run.start) - offset).length;
  const caught = `${maybe + likely}/${dos.length}  (${maybe} / ${likely})`;
  console.log(`${run.step.padEnd(18)} ${caught.padStart(34)} ${`${(alarmMs / Math.max(restMs, 1) * 100).toFixed(0)}% of ${(restMs / 1000).toFixed(0)} s`.padStart(22)}   ${moves}`);
}
if (process.argv.includes('--list')) for (const e of episodes) console.log(JSON.stringify(e));
