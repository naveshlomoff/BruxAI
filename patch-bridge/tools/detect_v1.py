#!/usr/bin/env python3
"""Bruxism detection rule, tried on calibration captures (see analyze_calibration.py). The reference
for patch-detector.js, which runs the same rule live in the app.

    python detect_v1.py <capture.jsonl>... [--profile temple|cheek]

Works on the bridge's ~10 Hz band frames with the pressure and motion samples resampled onto them,
using only past data for every baseline (so it could run live):
  sound  - 250-2000 Hz level above its quiet floor (20th percentile of the last 30 s). A frame
           counts as grinding-like unless it is voice-like (the <250 Hz band rose >9 dB too:
           speech, coughs, teeth tapping -- grinding stays under ~7) or broadband (250-2000 Hz rose
           less than 5 dB more than 2-4 kHz did: scratching or rubbing near the patch). Cue beeps
           are masked.
  pressure - deviation from the median of the last 5 s, and its 1 s spread (grinding with the
           cheek on the pillow shows as oscillation, clenching as a rise).
  motion - accelerometer spread (0.5 s) over its quiet floor: jaw-sized, or gross body movement.
Episodes (merged within 1.5 s) are graded: likely = grinding-like sound together with a pressure
event or jaw-sized motion, or (temple) a strong sustained clench; maybe = sound or pressure alone;
movement = mostly gross body movement, flagged separately.

Two profiles. 'cheek' is the first rule, tuned on the 28.09.2026 calibration with the patch over the
masseter. 'temple' (the default, the chosen placement) is tuned on the 30.09 + 04.10.2026
calibrations over the anterior temporalis, where the pressure sensor answers clenching in every
posture, the patch moves more with the jaw, and grinding sounds denser and lower-pitched.
Prints, for every calibration step, how many of its "do" phases were caught and how much of its
rest time raised a false alarm.
"""

import argparse

import numpy as np

import analyze_calibration as ac

FRAME_MS = 100

PROFILES = {
    'cheek': {
        'sound_excess_db': 8.0,      # 250-2000 Hz above its quiet floor
        'voice_low_db': 9.0,         # <250 Hz excess that marks voice / cough (speech p25 16, cough 13, grinding p75 <=7)
        'scratch_margin_db': 5.0,    # 250-2000 Hz rose less than this above 2-4 kHz -> broadband: scratch / rub
        'pressure_rise': 40.0,       # counts above the 5 s median
        'pressure_osc': 25.0,        # counts, 1 s standard deviation
        'press_min_frames': 3,
        'motion_jaw': (1.8, 4.5),    # motion spread over its floor: jaw-sized (grinding x2.5-4, touching the cheek x5.7)
        'motion_gross': 8.0,         # and body-sized
        'merge_ms': 1500,
        'sound_min_frames': 6,       # grinding-like frames in an episode (0.6 s of sound)...
        'sound_min_ms': 1500,        # ...spread over at least this long
        'voice_veto': 0.4,           # share of loud frames that are voice-like -> speech / cough (speech ~80%, grinding <25%)
        'scratch_veto': 0.4,         # share that are scratch-like -> touching near the patch
        'sound_min_density': None,   # (temple) see below
        'sound_min_tilt_db': None,
        'clench_rise': None,
        'clench_min_frames': None,
        'move_context': None,
        'move_context_ms': 5000,
    },
}
PROFILES['temple'] = dict(PROFILES['cheek'], **{
    # Clenching lifts the temple sensor 160-1000 counts over its 5 s median in every posture, while
    # drift at rest stays under ~85 and a clench's own edges swing far more than 1 s of rest (<40).
    'pressure_rise': 100.0,
    'pressure_osc': 50.0,
    'press_min_frames': 5,
    # Grinding moves the temple patch x4-8.5 (the cheek x2.5-4); body movement is x15-140.
    'motion_jaw': (1.8, 8.0),
    # Grinding fills >= 53% of the frames between its first and last grinding-like frame (all 13
    # temple episodes); a yawn 30%, tapping 28%.
    'sound_min_density': 0.45,
    # ...and is loudest at 500-1000 Hz: the 250-1000 Hz excess minus the 1000-2000 Hz excess has a
    # median of -3.4..+2.8 dB over its frames; a yawn peaks at 1-1.5 kHz (-9.3).
    'sound_min_tilt_db': -6.0,
    # A sustained clench (>= 0.8 s over pressure_rise, peak >= 120) is bruxism on its own here: a
    # half-force clench peaks at 162-277, on the back 267-328, on the side 486-995; the pressure of
    # grinding itself 21-118.
    'clench_rise': 120.0,
    'clench_min_frames': 8,
    # Within 5 s of a body movement (x15+: rolling over x26-60, getting up x27-45) "likely" drops to
    # "maybe": settling back into bed sounded like grinding (04.10).
    'move_context': 15.0,
})


def rolling_floor(values, times, span_ms, pct):
    """Percentile of the past `span_ms` for every point (causal)."""
    out = np.full(len(values), np.nan)
    j = 0
    for i in range(len(values)):
        while times[j] < times[i] - span_ms:
            j += 1
        window = values[j:i + 1]
        window = window[~np.isnan(window)]
        if len(window) >= 5:
            out[i] = np.percentile(window, pct)
    return out


def resample(t_src, v_src, t_dst, reducer, half_ms):
    out = np.full(len(t_dst), np.nan)
    if not len(t_src):
        return out
    order = np.argsort(t_src)
    t_src, v_src = t_src[order], v_src[order]
    lo = np.searchsorted(t_src, t_dst - half_ms)
    hi = np.searchsorted(t_src, t_dst + half_ms)
    for i, (a, b) in enumerate(zip(lo, hi)):
        if b - a >= 3:
            out[i] = reducer(v_src[a:b])
    return out


def detect(s, mic_lag, beep_times, profile='temple'):
    """Graded episodes, on the sensors' clock, for a series from ac.build_series."""
    p = PROFILES[profile]
    t = s['frame_t'] - mic_lag             # sound frames moved onto the sensors' clock
    bp = s['band_power']
    order = np.argsort(t)
    t, bp = t[order], bp[order]
    beep_mask = np.zeros(len(t), bool)
    for b in beep_times - mic_lag:
        beep_mask |= (t >= b + ac.BEEP_EXCLUDE_MS[0]) & (t <= b + ac.BEEP_EXCLUDE_MS[1])

    to_db = lambda x: 10 * np.log10(x + 1e-9)
    levels = {
        'brux': to_db(bp[:, 1:8].sum(axis=1)),          # 250-2000 Hz
        'low': to_db(bp[:, 0]),                         # <250 Hz
        'high': to_db(bp[:, [8, 9, 12, 13, 14]].sum(axis=1)),
        'lowmid': to_db(bp[:, 1:4].sum(axis=1)),        # 250-1000 Hz
        'highmid': to_db(bp[:, 4:8].sum(axis=1)),       # 1000-2000 Hz
    }
    x = {}
    for k, arr in levels.items():
        arr[beep_mask] = np.nan
        x[k] = arr - rolling_floor(arr, t, 30000, 20)
    brux_x, low_x, high_x = x['brux'], x['low'], x['high']
    tilt = x['lowmid'] - x['highmid']

    fsm = resample(s['fsm_t'], s['fsm'], t, np.mean, 60)
    fsm_dev = fsm - rolling_floor(fsm, t, 5000, 50)
    fsm_osc = resample(s['fsm_t'], s['fsm'], t, np.std, 500)
    acc_sd = resample(s['acc_t'], s['acc'], t, np.std, 250)
    acc_ratio = acc_sd / np.maximum(rolling_floor(acc_sd, t, 30000, 20), 1.0)

    loud = brux_x > p['sound_excess_db']
    voice = loud & (low_x > p['voice_low_db'])
    scratch = loud & ((brux_x - high_x) < p['scratch_margin_db']) & ~voice
    sound_hit = loud & ~voice & ~scratch
    rise = fsm_dev > p['pressure_rise']
    press_hit = rise | (fsm_osc > p['pressure_osc'])
    gross = acc_ratio > p['motion_gross']
    jaw = (acc_ratio > p['motion_jaw'][0]) & (acc_ratio <= p['motion_jaw'][1])

    # Episodes: stretches of sound or pressure hits, merged across short gaps.
    hit = np.nan_to_num(sound_hit.astype(float)) + np.nan_to_num(press_hit.astype(float)) > 0
    episodes, start, last = [], None, None
    for i in np.where(hit)[0]:
        if start is None:
            start = last = i
        elif t[i] - t[last] <= p['merge_ms']:
            last = i
        else:
            episodes.append((start, last))
            start = last = i
    if start is not None:
        episodes.append((start, last))

    graded = []
    for a, b in episodes:
        sl = slice(a, b + 1)
        n_sound, n_press = int(np.nansum(sound_hit[sl])), int(np.nansum(press_hit[sl]))
        n_voice, n_scratch = int(np.nansum(voice[sl])), int(np.nansum(scratch[sl]))
        loud_n = max(n_sound + n_voice + n_scratch, 1)
        duration = t[b] - t[a] + FRAME_MS
        is_gross = np.nanmean(gross[sl]) > 0.3
        # Grinding goes on for seconds; a swallow is one short sound. Speech and coughs keep some
        # voice-like frames even where single frames look like grinding -- veto the whole episode.
        sound_ok = n_sound >= p['sound_min_frames'] and duration >= p['sound_min_ms']
        if n_voice / loud_n >= p['voice_veto'] or n_scratch / loud_n >= p['scratch_veto']:
            sound_ok = False
        hits = a + np.where(sound_hit[sl])[0]
        density = len(hits) / (hits[-1] - hits[0] + 1) if len(hits) else 0.0
        tilt_med = float(np.median(tilt[hits])) if len(hits) else float('nan')
        if p['sound_min_density'] is not None and density < p['sound_min_density']:
            sound_ok = False
        if p['sound_min_tilt_db'] is not None and not tilt_med >= p['sound_min_tilt_db']:
            sound_ok = False
        press_ok = n_press >= p['press_min_frames']
        dev_max = float(np.nanmax(fsm_dev[sl])) if np.isfinite(fsm_dev[sl]).any() else float('nan')
        clench = (p['clench_rise'] is not None and int(np.nansum(rise[sl])) >= p['clench_min_frames']
                  and dev_max >= p['clench_rise'])
        if not sound_ok and not press_ok:
            continue
        kind = 'grind' if sound_ok else 'clench'
        if is_gross:
            level, kind = 'movement', None
        elif sound_ok and (press_ok or np.nanmean(jaw[sl]) > 0.3):
            level = 'likely'
        elif clench:
            level, kind = 'likely', 'clench'
        else:
            level = 'maybe'
        if level == 'likely' and p['move_context'] is not None:
            near = acc_ratio[(t >= t[a] - p['move_context_ms']) & (t <= t[b] + p['move_context_ms'])]
            if np.isfinite(near).any() and np.nanmax(near) > p['move_context']:
                level = 'maybe'
        graded.append({'t0': t[a], 't1': t[b], 'level': level, 'kind': kind, 'sound': n_sound, 'press': n_press,
                       'voice': n_voice, 'scratch': n_scratch, 'density': density, 'tilt': tilt_med,
                       'dev_max': dev_max})
    return graded


def score(graded, runs, offset):
    """Per calibration run: how many "do" phases an episode caught (maybe / likely), how much of the
    rest time was under a maybe/likely episode, and the movement episodes during the run."""
    rows = []
    for run in runs:
        dos = [x for x in run['phases'] if x['phase'] == 'do']
        caught_maybe = caught_likely = 0
        for x in dos:
            lo = x['r'] - offset - ac.PHONE_LATENCY_MS + 300
            hi = x['end_r'] - offset - ac.PHONE_LATENCY_MS + 800
            levels = [g['level'] for g in graded if g['t1'] >= lo and g['t0'] <= hi]
            if 'likely' in levels:
                caught_likely += 1
            elif 'maybe' in levels:
                caught_maybe += 1
        rest_ms = alarm_ms = 0
        for x in run['phases']:
            if x['phase'] in ('rest', 'lead', 'tail'):
                lo = x['r'] - offset - ac.PHONE_LATENCY_MS + 1500
                hi = x['end_r'] - offset - ac.PHONE_LATENCY_MS - 200
                if hi <= lo:
                    continue
                rest_ms += hi - lo
                for g in graded:
                    if g['level'] in ('maybe', 'likely'):
                        alarm_ms += max(0, min(hi, g['t1'] + FRAME_MS) - max(lo, g['t0']))
        moves = sum(1 for g in graded if g['level'] == 'movement'
                    and g['t1'] >= run['start_r'] - offset and g['t0'] <= run.get('end_r', run['start_r']) - offset)
        rows.append({'step': run['step'], 'dos': len(dos), 'maybe': caught_maybe, 'likely': caught_likely,
                     'rest_ms': rest_ms, 'alarm_ms': alarm_ms, 'moves': moves})
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('captures', nargs='+')
    ap.add_argument('--profile', choices=sorted(PROFILES), default='temple')
    args = ap.parse_args()
    flushes, calib = ac.load(*args.captures)
    s = ac.build_series(flushes)
    runs = ac.split_steps(calib)
    mic_lag, _, beep_times, _ = ac.detect_beeps(s, runs)
    print(f'{"step":18} {"do phases caught (maybe / likely)":>34} {"false alarms in rest":>22}   movement')
    for row in score(detect(s, mic_lag, beep_times, args.profile), runs, s['offset']):
        caught = f'{row["maybe"] + row["likely"]}/{row["dos"]}  ({row["maybe"]} / {row["likely"]})'
        alarm = f'{row["alarm_ms"] / max(row["rest_ms"], 1) * 100:.0f}% of {row["rest_ms"] / 1000:.0f} s'
        print(f'{row["step"]:18} {caught:>34} {alarm:>22}   {row["moves"]}')


if __name__ == '__main__':
    main()
