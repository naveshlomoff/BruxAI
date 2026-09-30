#!/usr/bin/env python3
"""Calibration analysis for a patch_capture.mjs recording (payload v2 + "calib" cue events).

    python analyze_calibration.py <capture.jsonl> [--out DIR] [--plots]

For every calibration step it lines the three sensors up on the bridge clock, cuts the recording
into the step's "do" and "rest" phases (from the cue events, shifted by the measured delays), and
reports how each sensor changed during the action:
  * sound  - power in the 16 x 250 Hz bands the bridge computes at the full 8 kHz; the analysis band
             is 250-2000 Hz (bands 1-7). Frames around each cue beep are dropped: the phone's beep
             (2.6-2.7 kHz) reaches the patch's mic and leaks a little into the band.
  * pressure - FSM samples, placed in time by their packet's arrival at the bridge.
  * motion - accelerometer magnitude (the patch sends |a| only), std within the window.
Writes <out>/calibration_summary.json (per step, per repetition) and, with --plots, PNG figures.
"""

import argparse
import base64
import json
import math
import os
import sys
from collections import defaultdict

import numpy as np

BAND_COUNT = 16
BAND_HZ = 250
FRAME_BYTES = 4 + BAND_COUNT
PACKET = 91
BRUX_BANDS = range(1, 8)       # 250-2000 Hz
BEEP_BAND = 10                 # 2500-2750 Hz: the cue beeps (2.6-2.7 kHz)
PHONE_LATENCY_MS = 100         # phone -> Supabase -> PC, assumed; cues are timed by PC receipt
REACTION_MS = 600              # start of a "do" window after its cue
RELEASE_MS = 300               # the action runs on a little past the rest cue
REST_WINDOW_MS = 2000          # reference: the last 2 s before each "do"
BEEP_EXCLUDE_MS = (-150, 350)  # sound frames dropped around each detected beep


def u16(b64):
    return np.frombuffer(base64.b64decode(b64 or ''), dtype='<u2').astype(float)


def load(path):
    flushes, calib = [], []
    with open(path, encoding='utf-8') as f:
        for line in f:
            if not line.strip():
                continue
            m = json.loads(line)
            if m['e'] == 'sample' and m['p'].get('v') == 2:
                flushes.append((m['r'], m['p']))
            elif m['e'] == 'calib':
                calib.append((m['r'], m['p']))
    return flushes, calib


def packet_sample_times(packet_times, counts):
    """Times for every sample of a channel sent in 91-sample packets: each packet's samples are
    spread back from its arrival, over the interval since the previous packet when that one arrived
    in sequence (else the typical interval)."""
    packet_times = np.asarray(packet_times, float)
    gaps = np.diff(packet_times)
    typical = np.median(gaps[(gaps > 300) & (gaps < 3000)]) if len(gaps) else 1000.0
    times = []
    for k, (t_arr, n) in enumerate(zip(packet_times, counts)):
        span = gaps[k - 1] if k > 0 and 0.6 * typical < gaps[k - 1] < 1.6 * typical else typical
        dt = span / PACKET
        times.append(t_arr - (n - 1 - np.arange(n)) * dt)
    return np.concatenate(times) if times else np.array([])


def build_series(flushes):
    offset = min(r - p['t'] for r, p in flushes)  # PC ms = bridge ms + offset (fastest path)
    frame_t, frame_codes = [], []
    fsm_vals, fsm_pkt_t, acc_vals, acc_pkt_t = [], [], [], []
    rms_t, rms_v = [], []
    for r, p in flushes:
        bf = base64.b64decode(p.get('bf') or '')
        for off in range(0, len(bf) - FRAME_BYTES + 1, FRAME_BYTES):
            frame_t.append(int.from_bytes(bf[off:off + 4], 'little'))
            frame_codes.append(list(bf[off + 4:off + FRAME_BYTES]))
        for key_vals, key_t, vals_list, t_list in (('fsm', 'ft', fsm_vals, fsm_pkt_t), ('acc', 'at', acc_vals, acc_pkt_t)):
            vals = u16(p.get(key_vals))
            pkts = p.get(key_t) or []
            if len(pkts) and len(vals) == len(pkts) * PACKET:
                for k, t_arr in enumerate(pkts):
                    vals_list.append(vals[k * PACKET:(k + 1) * PACKET])
                    t_list.append(t_arr)
        if isinstance(p.get('micRms'), (int, float)):
            rms_t.append(p['t'])
            rms_v.append(p['micRms'])
    codes = np.array(frame_codes, float)
    band_power = np.power(10.0, (codes - 40.0) / 20.0)  # counts^2, see encodeBandCode in the .ino
    series = {
        'offset': offset,
        'frame_t': np.array(frame_t, float),
        'band_power': band_power,
        'fsm_t': packet_sample_times(fsm_pkt_t, [len(v) for v in fsm_vals]),
        'fsm': np.concatenate(fsm_vals) if fsm_vals else np.array([]),
        'acc_t': packet_sample_times(acc_pkt_t, [len(v) for v in acc_vals]),
        'acc': np.concatenate(acc_vals) if acc_vals else np.array([]),
        'rms_t': np.array(rms_t, float),
        'rms': np.array(rms_v, float),
    }
    return series


def split_steps(calib):
    """One run per "start" event: its phases (in order) and its end result."""
    runs, current = [], None
    for r, p in sorted(calib, key=lambda x: x[0]):
        if p.get('type') == 'start':
            current = {'step': p['step'], 'session': p.get('session'), 'start_r': r, 'phases': [], 'result': None}
            runs.append(current)
        elif current and p.get('step') == current['step']:
            if p.get('type') == 'phase':
                current['phases'].append({'phase': p['phase'], 'rep': p.get('rep', 0), 'r': r})
            elif p.get('type') in ('end', 'abort'):
                current['end_r'] = r
                current['result'] = p.get('result')
                current['aborted'] = p.get('type') == 'abort'
                current = None
    for run in runs:
        ph = run['phases']
        for i, x in enumerate(ph):
            x['end_r'] = ph[i + 1]['r'] if i + 1 < len(ph) else run.get('end_r', x['r'])
    return [r for r in runs if not r.get('aborted') and r['phases']]


def db(x):
    return 10.0 * math.log10(max(x, 1e-9))


def detect_beeps(series, runs):
    """Every frame where the phone's 2.6-2.7 kHz beeps show (cues, countdown ticks, the rhythmic
    step's metronome), plus each cue's delay to its beep (ms, bridge clock) -- the mic path's lag."""
    t = series['frame_t']
    band_db = 10 * np.log10(series['band_power'] + 1e-9)
    # The tones (2620 / 2700 Hz) land in 2500-2750 Hz and, through the short analysis window, spill
    # into 2750-3000 Hz too. A beep is loud there and not in the bands on either side; a cough or a
    # scratch is loud at 2.5-3 kHz as well, but broadband -- those frames must stay in.
    beep = np.maximum(band_db[:, BEEP_BAND], band_db[:, BEEP_BAND + 1])
    floor = np.median(band_db[:, BEEP_BAND])
    neighbours = np.maximum(band_db[:, BEEP_BAND - 1], band_db[:, BEEP_BAND + 2])
    is_beep = (beep > floor + 15) & (beep - neighbours > 8)
    delays = []
    for run in runs:
        for x in run['phases']:
            if x['phase'] == 'lead':
                continue  # no beep at the start of the lead-in
            cue_b = x['r'] - series['offset'] - PHONE_LATENCY_MS
            sel = np.where((t >= cue_b - 300) & (t <= cue_b + 1500) & is_beep)[0]
            if len(sel):
                delays.append(t[sel[0]] - cue_b)
    return (float(np.median(delays)) if delays else 350.0), delays, t[is_beep], floor


def window_mask(times, lo, hi):
    return (times >= lo) & (times < hi)


def analyse_run(series, run, mic_lag, beep_times):
    offset = series['offset']
    ft, fsm = series['fsm_t'], series['fsm']
    at, acc = series['acc_t'], series['acc']
    frt, bp = series['frame_t'], series['band_power']
    keep = np.ones(len(frt), bool)
    for b in beep_times:
        keep &= ~((frt >= b + BEEP_EXCLUDE_MS[0]) & (frt <= b + BEEP_EXCLUDE_MS[1]))

    def sound(lo, hi):  # lo/hi: cue-time window on the bridge clock (sensor time)
        m = window_mask(frt, lo + mic_lag, hi + mic_lag) & keep
        if m.sum() < 2:
            return None
        return bp[m].mean(axis=0)

    reps = []
    phases = run['phases']
    for i, x in enumerate(phases):
        if x['phase'] != 'do':
            continue
        start = x['r'] - offset - PHONE_LATENCY_MS
        end = x['end_r'] - offset - PHONE_LATENCY_MS
        do_lo, do_hi = start + REACTION_MS, end + RELEASE_MS
        ref_lo, ref_hi = start - REST_WINDOW_MS - 100, start - 100
        s_do, s_ref = sound(do_lo, do_hi), sound(ref_lo, ref_hi)
        f_do, f_ref = fsm[window_mask(ft, do_lo, do_hi)], fsm[window_mask(ft, ref_lo, ref_hi)]
        a_do, a_ref = acc[window_mask(at, do_lo, do_hi)], acc[window_mask(at, ref_lo, ref_hi)]
        rep = {'rep': x['rep'], 'dur_s': (end - start) / 1000}
        if s_do is not None and s_ref is not None:
            rep['brux_db_do'] = db(s_do[list(BRUX_BANDS)].sum())
            rep['brux_db_ref'] = db(s_ref[list(BRUX_BANDS)].sum())
            rep['brux_db_delta'] = rep['brux_db_do'] - rep['brux_db_ref']
            rep['band_db_delta'] = [db(s_do[b]) - db(s_ref[b]) for b in range(BAND_COUNT)]
            rep['band_db_do'] = [db(s_do[b]) for b in range(BAND_COUNT)]
        if len(f_do) > 5 and len(f_ref) > 5:
            rep['fsm_do'] = float(np.mean(f_do))
            rep['fsm_ref'] = float(np.mean(f_ref))
            rep['fsm_delta'] = rep['fsm_do'] - rep['fsm_ref']
            rep['fsm_peak_delta'] = float(np.percentile(f_do, 95) - np.median(f_ref))
            rep['fsm_ref_sd'] = float(np.std(f_ref))
        if len(a_do) > 5 and len(a_ref) > 5:
            rep['acc_sd_do'] = float(np.std(a_do))
            rep['acc_sd_ref'] = float(np.std(a_ref))
            rep['acc_ratio'] = rep['acc_sd_do'] / max(rep['acc_sd_ref'], 1.0)
            rep['acc_mean_shift'] = float(np.mean(a_do) - np.mean(a_ref))
        reps.append(rep)
    return reps


def summarise(reps, key):
    vals = [r[key] for r in reps if key in r]
    if not vals:
        return None
    return {'median': float(np.median(vals)), 'min': float(np.min(vals)), 'max': float(np.max(vals)), 'n': len(vals)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('capture')
    ap.add_argument('--out', default=None)
    ap.add_argument('--plots', action='store_true')
    args = ap.parse_args()
    out_dir = args.out or os.path.join(os.path.dirname(os.path.abspath(args.capture)), 'analysis')
    os.makedirs(out_dir, exist_ok=True)

    flushes, calib = load(args.capture)
    series = build_series(flushes)
    runs = split_steps(calib)
    mic_lag, delays, beep_times, beep_floor = detect_beeps(series, runs)
    print(f'{len(flushes)} flushes, {len(runs)} calibration runs; fsm {len(series["fsm"])} samples, '
          f'acc {len(series["acc"])}, band frames {len(series["frame_t"])}')
    if delays:
        print(f'beep delay (mic path vs cue): median {mic_lag:.0f} ms, range {min(delays):.0f}..{max(delays):.0f} ms '
              f'over {len(delays)} cues; beep band floor {beep_floor:.1f} dB')

    summary = {'mic_lag_ms': mic_lag, 'beep_delays_ms': delays, 'steps': []}
    print(f'\n{"step":18} {"reps":>4} {"sound250-2k dB":>15} {"pressure":>16} {"press.noise":>11} {"motion x":>9}')
    for run in runs:
        reps = analyse_run(series, run, mic_lag, beep_times)
        entry = {
            'step': run['step'], 'session': run['session'], 'result_ok': (run['result'] or {}).get('ok'),
            'reps': reps,
            'brux_db_delta': summarise(reps, 'brux_db_delta'),
            'fsm_delta': summarise(reps, 'fsm_delta'),
            'fsm_peak_delta': summarise(reps, 'fsm_peak_delta'),
            'fsm_ref_sd': summarise(reps, 'fsm_ref_sd'),
            'acc_ratio': summarise(reps, 'acc_ratio'),
            'band_db_delta': (np.median([r['band_db_delta'] for r in reps if 'band_db_delta' in r], axis=0).tolist()
                              if any('band_db_delta' in r for r in reps) else None),
            'start_bridge_ms': run['start_r'] - series['offset'],
            'end_bridge_ms': run.get('end_r', run['start_r']) - series['offset'],
        }
        summary['steps'].append(entry)
        f = lambda s, fmt: (fmt.format(s['median']) + f' ({fmt.format(s["min"])}..{fmt.format(s["max"])})') if s else '-'
        print(f'{run["step"]:18} {len(reps):>4} {f(entry["brux_db_delta"], "{:+.1f}"):>15} '
              f'{f(entry["fsm_delta"], "{:+.0f}"):>16} {(("{:.0f}".format(entry["fsm_ref_sd"]["median"])) if entry["fsm_ref_sd"] else "-"):>11} '
              f'{(("{:.1f}".format(entry["acc_ratio"]["median"])) if entry["acc_ratio"] else "-"):>9}')

    with open(os.path.join(out_dir, 'calibration_summary.json'), 'w', encoding='utf-8') as fh:
        json.dump(summary, fh, indent=1)

    print('\nexcess sound per 250 Hz band during the action (dB, median of reps; bands 0..15 = 0-4 kHz):')
    for e in summary['steps']:
        if e['band_db_delta']:
            print(f'{e["step"]:18} ' + ' '.join(f'{v:+5.1f}' for v in e['band_db_delta']))

    if args.plots:
        plot(series, runs, summary, mic_lag, beep_times, out_dir)
        print(f'\nplots in {out_dir}')


def plot(series, runs, summary, mic_lag, beep_times, out_dir):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt

    offset = series['offset']
    frt, bp = series['frame_t'], series['band_power']
    brux = 10 * np.log10(bp[:, list(BRUX_BANDS)].sum(axis=1) + 1e-9)
    high = 10 * np.log10(bp[:, [8, 9, 11, 12, 13, 14]].sum(axis=1) + 1e-9)
    n = len(runs)
    cols = 2
    rows = math.ceil(n / cols)
    fig, axes = plt.subplots(rows, cols, figsize=(15, 2.6 * rows), squeeze=False)
    for ax_i, run in enumerate(runs):
        ax = axes[ax_i // cols][ax_i % cols]
        t0 = run['start_r'] - offset - PHONE_LATENCY_MS
        t1 = run.get('end_r', run['start_r']) - offset
        m = (frt >= t0 - 1000) & (frt <= t1 + 1000)
        ax.plot((frt[m] - t0 - mic_lag) / 1000, brux[m], lw=0.8, color='#4f46e5', label='sound 250-2000 Hz (dB)')
        ax2 = ax.twinx()
        mf = (series['fsm_t'] >= t0 - 1000) & (series['fsm_t'] <= t1 + 1000)
        ax2.plot((series['fsm_t'][mf] - t0) / 1000, series['fsm'][mf], lw=0.8, color='#dc2626', label='pressure')
        ma = (series['acc_t'] >= t0 - 1000) & (series['acc_t'] <= t1 + 1000)
        if ma.any():
            acc = series['acc'][ma]
            acc_n = (acc - np.median(acc)) / 40 + np.median(series['fsm'][mf]) if mf.any() else acc
            ax2.plot((series['acc_t'][ma] - t0) / 1000, acc_n, lw=0.5, color='#059669', alpha=0.7, label='motion (scaled)')
        for x in run['phases']:
            if x['phase'] == 'do':
                ax.axvspan((x['r'] - offset - PHONE_LATENCY_MS - t0) / 1000, (x['end_r'] - offset - PHONE_LATENCY_MS - t0) / 1000,
                           color='#f59e0b', alpha=0.18)
        ax.set_title(run['step'], fontsize=9)
        ax.tick_params(labelsize=7)
        ax2.tick_params(labelsize=7, colors='#dc2626')
    for k in range(n, rows * cols):
        axes[k // cols][k % cols].axis('off')
    fig.tight_layout()
    fig.savefig(os.path.join(out_dir, 'steps_timeseries.png'), dpi=110)
    plt.close(fig)

    fig, ax = plt.subplots(figsize=(11, 5))
    freqs = np.arange(BAND_COUNT) * BAND_HZ + BAND_HZ / 2
    for e in summary['steps']:
        if e['band_db_delta'] and e['step'].removeprefix('t-') in ('grind', 'bed-back-grind', 'bed-side-grind', 'tap', 'speech', 'swallow', 'cough', 'touch', 'yawn'):
            style = '-' if 'grind' in e['step'] else '--'
            ax.plot(freqs, e['band_db_delta'], style, marker='o', ms=3, label=e['step'])
    ax.axvspan(250, 2000, color='#6366f1', alpha=0.08)
    ax.axhline(0, color='k', lw=0.5)
    ax.set_xlabel('Hz')
    ax.set_ylabel('dB above the preceding rest')
    ax.legend(fontsize=8, ncol=3)
    fig.tight_layout()
    fig.savefig(os.path.join(out_dir, 'sound_signatures.png'), dpi=110)
    plt.close(fig)


if __name__ == '__main__':
    main()
