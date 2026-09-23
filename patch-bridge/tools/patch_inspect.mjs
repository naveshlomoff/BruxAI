#!/usr/bin/env node
// Sanity report for a patch_capture.mjs file: flush gaps, sample rates, packet timing, and whether the
// bridge's sound bands add up to its full-rate micRms (they should: both measure the same variance).
//
//   node patch_inspect.mjs <capture.jsonl>

import fs from 'node:fs';

const BAND_COUNT = 16;
const FRAME_BYTES = 4 + BAND_COUNT;
const file = process.argv[2];
if (!file) { console.error('usage: node patch_inspect.mjs <capture.jsonl>'); process.exit(1); }

const u16 = (b64) => {
  const buf = Buffer.from(b64 || '', 'base64');
  return Array.from({ length: buf.length >> 1 }, (_, i) => buf.readUInt16LE(i * 2));
};
const codeToPower = (c) => Math.pow(10, (c - 40) / 20);
const pct = (arr, p) => { const s = [...arr].sort((a, b) => a - b); return s[Math.min(s.length - 1, Math.floor(s.length * p))]; };

const lines = fs.readFileSync(file, 'utf8').split('\n').filter(Boolean).map((l) => JSON.parse(l));
const flushes = lines.filter((l) => l.e === 'sample' && l.p.v === 2);
const v1 = lines.filter((l) => l.e === 'sample' && l.p.v !== 2).length;
console.log(`${lines.length} messages, ${flushes.length} v2 flushes, ${v1} v1 flushes`);
if (!flushes.length) process.exit(0);

let gaps = 0, micN = 0, accN = 0, fsmN = 0, accPk = 0, fsmPk = 0, frames = 0, micPackets = 0, dropped = 0;
let misaligned = 0, nonMonotonic = 0, lastFrameT = 0;
const offsets = [], ratios = [], bruxDb = [], interFlush = [];
for (let i = 0; i < flushes.length; i++) {
  const { r, p } = flushes[i];
  if (i) {
    const prev = flushes[i - 1].p;
    if (p.q > prev.q + 1) gaps += p.q - prev.q - 1;
    interFlush.push(p.t - prev.t);
  }
  offsets.push(r - p.t);
  const mic = u16(p.mic), acc = u16(p.acc), fsm = u16(p.fsm);
  micN += mic.length; accN += acc.length; fsmN += fsm.length;
  accPk += (p.at || []).length; fsmPk += (p.ft || []).length;
  if (acc.length !== (p.at || []).length * 91 || fsm.length !== (p.ft || []).length * 91) misaligned++;
  micPackets += p.mp || 0;
  dropped += p.d || 0;
  const bf = Buffer.from(p.bf || '', 'base64');
  let flushPower = 0, n = 0;
  for (let off = 0; off + FRAME_BYTES <= bf.length; off += FRAME_BYTES) {
    const t = bf.readUInt32LE(off);
    if (t < lastFrameT) nonMonotonic++;
    lastFrameT = t;
    let total = 0, brux = 0;
    for (let b = 0; b < BAND_COUNT; b++) {
      const pw = codeToPower(bf[off + 4 + b]);
      total += pw;
      if (b >= 1 && b <= 7) brux += pw;
    }
    flushPower += total; n++;
    bruxDb.push(10 * Math.log10(brux));
    frames++;
  }
  if (n && p.micRms) ratios.push(Math.sqrt(flushPower / n) / p.micRms);
}

const spanS = (flushes.at(-1).p.t - flushes[0].p.t) / 1000;
console.log(`span ${spanS.toFixed(0)} s on the bridge clock; flush sequence gaps: ${gaps}; bridge-side drops: ${dropped}`);
console.log(`flush interval ms: median ${pct(interFlush, 0.5)}, p99 ${pct(interFlush, 0.99)}, max ${Math.max(...interFlush)}`);
console.log(`rates/s: mic relay ${(micN / spanS).toFixed(0)}, mic packets ${(micPackets / spanS).toFixed(1)} (patch makes 87.9), acc ${(accN / spanS).toFixed(1)}, fsm ${(fsmN / spanS).toFixed(1)}, band frames ${(frames / spanS).toFixed(1)}`);
console.log(`acc/fsm packets with arrival times: ${accPk}/${fsmPk}; flushes where samples != packets*91: ${misaligned}; frame times out of order: ${nonMonotonic}`);
console.log(`PC receive minus bridge clock (ms): min ${Math.min(...offsets)}, median ${pct(offsets, 0.5)}, p95 ${pct(offsets, 0.95)} -> latency spread ${pct(offsets, 0.95) - Math.min(...offsets)} ms`);
console.log(`sqrt(sum of band powers) / micRms per flush: median ${pct(ratios, 0.5).toFixed(3)} (p5 ${pct(ratios, 0.05).toFixed(3)}, p95 ${pct(ratios, 0.95).toFixed(3)}) -- ~0.9-1.0 expected`);
console.log(`250-2000 Hz band level dB (counts^2): p10 ${pct(bruxDb, 0.1).toFixed(1)}, median ${pct(bruxDb, 0.5).toFixed(1)}, p90 ${pct(bruxDb, 0.9).toFixed(1)}, max ${Math.max(...bruxDb).toFixed(1)}`);

// Average spectrum, to eyeball the mic's idle noise shape.
const avg = new Array(BAND_COUNT).fill(0);
let count = 0;
for (const { p } of flushes) {
  const bf = Buffer.from(p.bf || '', 'base64');
  for (let off = 0; off + FRAME_BYTES <= bf.length; off += FRAME_BYTES) {
    for (let b = 0; b < BAND_COUNT; b++) avg[b] += codeToPower(bf[off + 4 + b]);
    count++;
  }
}
console.log('mean band level dB, 0-4 kHz in 250 Hz steps:', avg.map((v) => (10 * Math.log10(v / count)).toFixed(1)).join(' '));
