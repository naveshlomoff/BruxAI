#!/usr/bin/env python3
"""Cheek vs temple: the same calibration steps recorded with the patch in two places, side by side.

    python compare_placements.py <cheek capture.jsonl> <temple capture.jsonl> [--out DIR] [--plots]

Steps are matched by id (the temple protocol's ids are the cheek ones with a "t-" prefix); a step
that was run more than once counts by its last completed run. Per step and placement:
  sound     - 250-2000 Hz rise during the action (dB), and the <250 Hz rise, the voice marker
              (speech, coughs and tapping raise it, grinding hardly does)
  pressure  - mean rise during the action, the rest-time noise (sd) and their ratio
  motion    - accelerometer spread, action over rest
  detection - detect_v1 over the whole capture: "do" phases caught (likely + maybe of all) and the
              share of rest time under a false alarm
  link      - sound frames per second and pressure samples per second that reached the bridge
Writes <out>/placement_comparison.json and, with --plots, placement_comparison.png and
placement_pressure.png.
"""

import argparse
import json
import os

import numpy as np

import analyze_calibration as ac
import detect_v1 as dv

STEP_ORDER = ['placement', 'mvc', 'silence', 'clench50', 'rhythmic', 'grind', 'tap', 'speech', 'swallow',
              'cough', 'yawn', 'touch', 'head', 'bed-back-clench', 'bed-back-grind', 'bed-side-clench',
              'bed-side-grind', 'roll', 'getup']
PLACEMENTS = ('cheek', 'temple')


def base_id(step):
    return step[2:] if step.startswith('t-') else step


def median(reps, key, pick=None):
    vals = [pick(r[key]) if pick else r[key] for r in reps if key in r]
    return float(np.median(vals)) if vals else None


def analyse_capture(path):
    flushes, calib = ac.load(path)
    s = ac.build_series(flushes)
    runs = ac.split_steps(calib)
    mic_lag, _, beep_times, _ = ac.detect_beeps(s, runs)
    detection = dv.score(dv.detect(s, mic_lag, beep_times), runs, s['offset'])
    steps = {}
    for run, det in zip(runs, detection):  # later runs of a step replace earlier ones
        reps = ac.analyse_run(s, run, mic_lag, beep_times)
        rates = (run['result'] or {}).get('rates') or {}
        press, noise = median(reps, 'fsm_delta'), median(reps, 'fsm_ref_sd')
        steps[base_id(run['step'])] = {
            'runs': steps.get(base_id(run['step']), {}).get('runs', 0) + 1,
            'reps': len(reps),
            'sound_db': median(reps, 'brux_db_delta'),
            'low_db': median(reps, 'band_db_delta', lambda v: v[0]),
            'band_db_delta': (np.median([r['band_db_delta'] for r in reps if 'band_db_delta' in r], axis=0).tolist()
                              if any('band_db_delta' in r for r in reps) else None),
            'pressure': press,
            'pressure_peak': median(reps, 'fsm_peak_delta'),
            'pressure_noise': noise,
            'pressure_snr': abs(press) / max(noise, 1.0) if press is not None and noise is not None else None,
            'motion': median(reps, 'acc_ratio'),
            'dos': det['dos'], 'likely': det['likely'], 'maybe': det['maybe'],
            'false_alarm': det['alarm_ms'] / det['rest_ms'] if det['rest_ms'] else None,
            'rest_s': det['rest_ms'] / 1000,
            'frames_per_s': rates.get('frames'), 'fsm_hz': rates.get('fsm'),
        }
    return {'mic_lag_ms': mic_lag, 'steps': steps, 'series': s, 'runs': runs}


def fmt(v, spec, width):
    return (spec.format(v) if v is not None else '-').rjust(width)


def print_table(res):
    cols = [('sound dB', 'sound_db', '{:+.1f}', 6), ('<250Hz dB', 'low_db', '{:+.1f}', 6),
            ('pressure', 'pressure', '{:+.0f}', 6), ('p.noise', 'pressure_noise', '{:.0f}', 5),
            ('p.snr', 'pressure_snr', '{:.1f}', 5), ('motion x', 'motion', '{:.1f}', 5)]
    head = f'{"step":16}' + ''.join(f' | {name:^{2 * w + 1}}' for name, _, _, w in cols) + \
        f' | {"caught L+M/N":^15} | {"false alarm":^11} | {"frames/s":^11} | {"fsm Hz":^9}'
    sub = f'{"":16}' + ''.join(f' | {"cheek":>{w}} {"temp.":>{w}}' for _, _, _, w in cols) + \
        f' | {"cheek":>7} {"temp.":>7} | {"cheek":>5} {"temp.":>5} | {"cheek":>5} {"temp.":>5} | {"cheek":>4} {"temp.":>4}'
    print(head)
    print(sub)
    for step in STEP_ORDER:
        e = [res[p]['steps'].get(step) for p in PLACEMENTS]
        if not any(e):
            continue
        line = f'{step:16}'
        for _, key, spec, w in cols:
            line += ' | ' + ' '.join(fmt(x.get(key) if x else None, spec, w) for x in e)
        caught = [f'{x["likely"]}+{x["maybe"]}/{x["dos"]}' if x else '-' for x in e]
        alarm = [fmt(x['false_alarm'] * 100 if x and x['false_alarm'] is not None else None, '{:.0f}%', 5) for x in e]
        frames = [fmt(x.get('frames_per_s') if x else None, '{:.1f}', 5) for x in e]
        fsm = [fmt(x.get('fsm_hz') if x else None, '{:.0f}', 4) for x in e]
        line += f' | {caught[0]:>7} {caught[1]:>7} | {alarm[0]} {alarm[1]} | {frames[0]} {frames[1]} | {fsm[0]} {fsm[1]}'
        print(line)


def plot(res, out_dir):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt

    colors = {'cheek': '#6366f1', 'temple': '#f59e0b'}

    def bars(ax, steps, key, title, ylabel, scale=1.0, log=None):  # log: symlog's linear range
        steps = [s for s in steps if any(res[p]['steps'].get(s) for p in PLACEMENTS)]
        x = np.arange(len(steps))
        for k, p in enumerate(PLACEMENTS):
            vals = [(res[p]['steps'].get(s) or {}).get(key) for s in steps]
            ax.bar(x + (k - 0.5) * 0.4, [v * scale if v is not None else 0 for v in vals], 0.4,
                   color=colors[p], label=p)
            for xi, v in zip(x, vals):
                if v is None:
                    ax.text(xi + (k - 0.5) * 0.4, 0, 'n/a', ha='center', va='bottom', fontsize=6, color='#888')
        ax.set_xticks(x)
        ax.set_xticklabels(steps, rotation=35, ha='right', fontsize=7)
        ax.set_title(title, fontsize=9)
        ax.set_ylabel(ylabel, fontsize=8)
        ax.axhline(0, color='k', lw=0.5)
        if log:
            ax.set_yscale('symlog', linthresh=log)
        ax.tick_params(labelsize=7)
        ax.legend(fontsize=7)

    brux = ['grind', 'bed-back-grind', 'bed-side-grind', 'rhythmic', 'clench50', 'mvc']
    confound = ['speech', 'cough', 'tap', 'swallow', 'yawn', 'touch', 'head']
    fig, axes = plt.subplots(3, 2, figsize=(14, 11))
    bars(axes[0][0], brux + confound, 'sound_db', 'Sound 250-2000 Hz: rise during the action', 'dB')
    bars(axes[0][1], brux + confound, 'low_db', 'Sound < 250 Hz (voice marker): rise during the action', 'dB')
    clench = ['placement', 'mvc', 'clench50', 'rhythmic', 'grind', 'bed-back-clench', 'bed-back-grind',
              'bed-side-clench', 'bed-side-grind']
    bars(axes[1][0], clench, 'pressure', 'Pressure: rise during the action', 'counts', log=10)
    bars(axes[1][1], clench, 'pressure_snr', 'Pressure: rise / rest noise', 'x', log=1)
    bars(axes[2][0], STEP_ORDER, 'motion', 'Motion: spread during the action / rest', 'x', log=1)
    detect_steps = brux + confound + ['roll', 'getup']
    for p in PLACEMENTS:
        for s in detect_steps:
            e = res[p]['steps'].get(s)
            if e:
                e['caught_share'] = (e['likely'] + e['maybe']) / e['dos'] if e['dos'] else None
    bars(axes[2][1], detect_steps, 'caught_share', 'Detection v1 (cheek thresholds): share of actions flagged', 'share')
    fig.tight_layout()
    fig.savefig(os.path.join(out_dir, 'placement_comparison.png'), dpi=110)
    plt.close(fig)

    # Pressure traces of the clench / grind steps: same time axis and, per row, the same pressure
    # scale for both placements, so the size of the response compares directly.
    show = ['placement', 'clench50', 'mvc', 'grind', 'bed-back-clench', 'bed-back-grind', 'bed-side-clench', 'bed-side-grind']
    last = {p: {base_id(run['step']): run for run in res[p]['runs']} for p in PLACEMENTS}
    fig, axes = plt.subplots(len(show), 2, figsize=(14, 2.1 * len(show)), squeeze=False)
    for row, step in enumerate(show):
        traces = {}
        for p in PLACEMENTS:
            s, run = res[p]['series'], last[p].get(step)
            if not run:
                continue
            t0 = run['start_r'] - s['offset'] - ac.PHONE_LATENCY_MS
            t1 = run.get('end_r', run['start_r']) - s['offset']
            m = (s['fsm_t'] >= t0) & (s['fsm_t'] <= t1)
            t, v = (s['fsm_t'][m] - t0) / 1000, s['fsm'][m]
            spans = [((x['r'] - s['offset'] - ac.PHONE_LATENCY_MS - t0) / 1000,
                      (x['end_r'] - s['offset'] - ac.PHONE_LATENCY_MS - t0) / 1000) for x in run['phases'] if x['phase'] == 'do']
            rest = np.ones(len(t), bool)
            for a, b in spans:
                rest &= ~((t >= a) & (t < b + 1.0))
            base = np.median(v[rest]) if rest.any() else (np.median(v) if len(v) else 0)
            traces[p] = (t, v - base, spans)
        values = np.concatenate([tr[1] for tr in traces.values()]) if traces else np.array([])
        lim = (np.percentile(values, 0.5), np.percentile(values, 99.5)) if len(values) else (-1, 1)
        pad = 0.08 * (lim[1] - lim[0] + 1)
        for col, p in enumerate(PLACEMENTS):
            ax = axes[row][col]
            ax.set_title(f'{p}: {step}', fontsize=8)
            ax.tick_params(labelsize=7)
            if p not in traces:
                ax.text(0.5, 0.5, 'not recorded', transform=ax.transAxes, ha='center', fontsize=8, color='#888')
                continue
            t, v, spans = traces[p]
            ax.plot(t, v, '.', ms=1.5, color=colors[p])
            for a, b in spans:
                ax.axvspan(a, b, color='#10b981', alpha=0.15)
            ax.set_ylim(lim[0] - pad, lim[1] + pad)
            ax.set_ylabel('pressure - rest', fontsize=7)
    fig.tight_layout()
    fig.savefig(os.path.join(out_dir, 'placement_pressure.png'), dpi=110)
    plt.close(fig)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cheek')
    ap.add_argument('temple')
    ap.add_argument('--out', default=None)
    ap.add_argument('--plots', action='store_true')
    args = ap.parse_args()
    out_dir = args.out or os.path.join(os.path.dirname(os.path.abspath(args.temple)), 'analysis-placement')
    os.makedirs(out_dir, exist_ok=True)

    res = {'cheek': analyse_capture(args.cheek), 'temple': analyse_capture(args.temple)}
    for p in PLACEMENTS:
        print(f'{p}: {len(res[p]["runs"])} runs, mic lag {res[p]["mic_lag_ms"]:.0f} ms')
    print()
    print_table(res)
    with open(os.path.join(out_dir, 'placement_comparison.json'), 'w', encoding='utf-8') as fh:
        json.dump({p: {'mic_lag_ms': res[p]['mic_lag_ms'], 'steps': res[p]['steps']} for p in PLACEMENTS}, fh, indent=1)
    if args.plots:
        plot(res, out_dir)
        print(f'\nplots in {out_dir}')


if __name__ == '__main__':
    main()
