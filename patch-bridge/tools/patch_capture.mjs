#!/usr/bin/env node
// Records everything on the Supabase Realtime "patch-live" channel -- the bridge's samples and
// status, the app's commands and calibration cues -- to a JSONL file, one message per line with this
// PC's receive time. Calibration runs are analysed from these files: the app's own cloud copies are
// per-user (RLS), so this is the way to get the data onto a PC as it happens. Node 22+ (global
// WebSocket), no dependencies.
//
//   node patch_capture.mjs [--out DIR] [--prefix NAME] [--minutes N] [--drive]
//
//   --out DIR    where to write (default %USERPROFILE%\BruxAI-data\patch-capture). Keep recordings
//                out of the repo: it is public, and this is body data.
//   --prefix NAME  file name prefix (default "capture"; the night launcher uses "night")
//   --minutes N  stop after N minutes (default: run until Ctrl+C)
//   --drive      also act as the app: send connect (+ keepalive every 15 s) and disconnect on exit,
//                and connect again whenever the bridge reports idle (it restarted, or a page sent
//                Disconnect) -- so stop it before pressing Disconnect in the app. For bench tests and
//                overnight recordings with no phone; leave it off while the phone runs a calibration.
//
// Prints a one-line health summary every 10 s (flush gaps, samples per sensor) and one line per
// calibration cue, so a run can be followed live.

import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';

const HOST = 'ukoswihzqpztfypqhdnc.supabase.co';
const KEY = 'sb_publishable_VTSRe2BT1ppguJKRhwQF3A_xr0rUtBt'; // publishable, already in index.html
const TOPIC = 'realtime:patch-live';
const BAND_COUNT = 16; // must match BAND_COUNT in BroxMonBridge.ino

const args = process.argv.slice(2);
const argValue = (name, fallback) => {
  const i = args.indexOf(name);
  return i >= 0 && args[i + 1] ? args[i + 1] : fallback;
};
const outDir = argValue('--out', path.join(os.homedir(), 'BruxAI-data', 'patch-capture'));
const minutes = Number(argValue('--minutes', '0'));
const drive = args.includes('--drive');
const prefix = argValue('--prefix', 'capture');

fs.mkdirSync(outDir, { recursive: true });
const stamp = new Date().toISOString().replace(/[-:]/g, '').replace('T', '-').slice(0, 15);
const outFile = path.join(outDir, `${prefix}-${stamp}.jsonl`);
const out = fs.createWriteStream(outFile, { flags: 'a' });
console.log(`[capture] writing ${outFile}${drive ? ' (driving the bridge)' : ''}`);

let ws = null;
let ref = 1;
let joined = false;
let heartbeatTimer = null;
let keepaliveTimer = null;
let closing = false;

// ── rolling stats for the 10 s summary ──
const stats = { flushes: 0, gaps: 0, v1: 0, mic: 0, acc: 0, fsm: 0, frames: 0, maxGapMs: 0, lastRecv: 0, state: '?' };
let lastSeq = null;
let patchWasConnected = false;
let lastConnectAt = 0;
let bandSum = new Array(BAND_COUNT).fill(0);

function send(msg) {
  if (ws && ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(msg));
}

function broadcast(event, payload) {
  send({ topic: TOPIC, event: 'broadcast', payload: { type: 'broadcast', event, payload }, ref: String(++ref), join_ref: '1' });
}

function command(action) {
  broadcast('command', { action, fmt: 2 });
}

// Uint16 little-endian samples from the bridge's base64 (payload v2).
function decodeU16(b64) {
  const buf = Buffer.from(b64 || '', 'base64');
  const outArr = new Array(buf.length >> 1);
  for (let i = 0; i < outArr.length; i++) outArr[i] = buf.readUInt16LE(i * 2);
  return outArr;
}

function onSample(p, recv) {
  stats.flushes++;
  if (stats.lastRecv) stats.maxGapMs = Math.max(stats.maxGapMs, recv - stats.lastRecv);
  stats.lastRecv = recv;
  if (p.v === 2) {
    if (lastSeq !== null && p.q > lastSeq + 1) stats.gaps += p.q - lastSeq - 1;
    lastSeq = p.q;
    stats.mic += decodeU16(p.mic).length;
    stats.acc += decodeU16(p.acc).length;
    stats.fsm += decodeU16(p.fsm).length;
    const frames = Buffer.from(p.bf || '', 'base64');
    const frameBytes = 4 + BAND_COUNT;
    for (let off = 0; off + frameBytes <= frames.length; off += frameBytes) {
      stats.frames++;
      for (let b = 0; b < BAND_COUNT; b++) bandSum[b] += frames[off + 4 + b];
    }
  } else {
    stats.v1++;
    stats.mic += (p.mic || []).length;
    stats.acc += (p.acc || []).length;
    stats.fsm += (p.fsm || []).length;
  }
}

// Band codes are 0.5 dB steps: power = 10^((code - 40) / 20) counts^2 (see encodeBandFrame in the .ino).
function bandGroupDb(from, to) {
  if (!stats.frames) return '-';
  let power = 0;
  for (let b = from; b <= to; b++) power += Math.pow(10, (bandSum[b] / stats.frames - 40) / 20);
  return (10 * Math.log10(power)).toFixed(1);
}

function printStats() {
  const line = `[stats] ${new Date().toLocaleTimeString()} state=${stats.state} flushes=${stats.flushes}` +
    (stats.v1 ? ` (v1 payloads ${stats.v1})` : ` seqGaps=${stats.gaps}`) +
    ` maxGap=${(stats.maxGapMs / 1000).toFixed(1)}s mic=${stats.mic} acc=${stats.acc} fsm=${stats.fsm}` +
    (stats.frames ? ` frames=${stats.frames} dB low/brux/high=${bandGroupDb(0, 0)}/${bandGroupDb(1, 7)}/${bandGroupDb(8, 15)}` : '');
  console.log(line);
  Object.assign(stats, { flushes: 0, gaps: 0, v1: 0, mic: 0, acc: 0, fsm: 0, frames: 0, maxGapMs: 0 });
  bandSum = new Array(BAND_COUNT).fill(0);
}

function connect() {
  const url = `wss://${HOST}/realtime/v1/websocket?apikey=${KEY}&vsn=1.0.0`;
  ws = new WebSocket(url);
  ws.onopen = () => {
    send({
      topic: TOPIC, event: 'phx_join', ref: '1', join_ref: '1',
      payload: { config: { broadcast: { ack: false, self: false }, presence: { key: '' }, postgres_changes: [], private: false } },
    });
    heartbeatTimer = setInterval(() => send({ topic: 'phoenix', event: 'heartbeat', payload: {}, ref: String(++ref) }), 25000);
  };
  ws.onmessage = (m) => {
    const recv = Date.now();
    let msg;
    try { msg = JSON.parse(m.data); } catch { return; }
    if (msg.event === 'phx_reply' && msg.ref === '1') {
      joined = msg.payload?.status === 'ok';
      console.log(`[capture] channel join ${joined ? 'ok' : 'refused'}`);
      if (joined && drive) {
        command('connect');
        lastConnectAt = Date.now();
        clearInterval(keepaliveTimer);
        keepaliveTimer = setInterval(() => command('keepalive'), 15000);
      }
      return;
    }
    if (msg.event !== 'broadcast') return;
    const event = msg.payload?.event;
    const p = msg.payload?.payload || {};
    out.write(JSON.stringify({ r: recv, e: event, p }) + '\n');
    if (event === 'sample') onSample(p, recv);
    else if (event === 'status') {
      stats.state = p.state || (p.patchConnected ? 'connected' : 'idle');
      // "-> connecting" is a drop (the bridge went back to scanning), "-> idle" a Disconnect. disc is
      // the bridge's last disconnect reason (fw 2026-09-30+): 0x208 = the link timed out.
      if (patchWasConnected && !p.patchConnected) {
        console.log(`[link] ${new Date(recv).toLocaleTimeString()} patch link ended -> ${stats.state}` +
          (p.disc ? ` (reason 0x${p.disc.toString(16)})` : ''));
      }
      patchWasConnected = !!p.patchConnected;
      // Driving: a bridge that restarted, or got a Disconnect from a page, reports idle -- ask again.
      if (drive && joined && stats.state === 'idle' && recv - lastConnectAt > 30000) {
        console.log(`[drive] ${new Date(recv).toLocaleTimeString()} bridge idle -- sending connect`);
        command('connect');
        lastConnectAt = recv;
      }
    }
    else if (event === 'calib') {
      const what = p.type === 'phase' ? `${p.phase} ${p.action || ''} #${p.rep ?? ''}` : p.type;
      console.log(`[calib] ${new Date(recv).toLocaleTimeString()} ${p.step} ${what}${p.result ? ' ' + JSON.stringify(p.result) : ''}`);
    }
  };
  ws.onclose = () => {
    joined = false;
    clearInterval(heartbeatTimer);
    clearInterval(keepaliveTimer);
    if (closing) return;
    console.log('[capture] socket closed -- reconnecting in 3 s');
    setTimeout(connect, 3000);
  };
  ws.onerror = () => {};
}

function shutdown() {
  if (closing) return;
  closing = true;
  if (drive && joined) command('disconnect');
  setTimeout(() => {
    try { ws?.close(); } catch {}
    out.end(() => {
      console.log(`[capture] saved ${outFile}`);
      process.exit(0);
    });
  }, 800);
}

setInterval(printStats, 10000);
process.on('SIGINT', shutdown);
process.on('SIGTERM', shutdown);
process.on('SIGHUP', shutdown); // Windows: the console window was closed
if (minutes > 0) setTimeout(shutdown, minutes * 60000);
connect();
