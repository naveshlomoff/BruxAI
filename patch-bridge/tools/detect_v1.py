#!/usr/bin/env python3
"""First bruxism detection rule, tried on a calibration capture (see analyze_calibration.py).

    python detect_v1.py <capture.jsonl>

Works on the bridge's ~10 Hz band frames with the pressure and motion samples resampled onto them,
using only past data for every baseline (so it could run live):
  sound  - 250-2000 Hz level above its quiet floor (20th percentile of the last 30 s). A frame
           counts as grinding-like unless it is voice-like (the <250 Hz band rose >9 dB too:
           speech, coughs, teeth tapping -- grinding stays under ~7) or broadband (250-2000 Hz rose
           less than 5 dB more than 2-4 kHz did: scratching or rubbing near the patch). Cue beeps
           are masked.
  pressure - deviation from the median of the last 5 s, and its 1 s spread (grinding with the
           cheek on the pillow shows as oscillation, clenching as a rise).
  motion - accelerometer spread (0.5 s) over its quiet floor; x8 and up is gross body movement.
Episodes (merged within 1.5 s) are graded: likely = grinding-like sound together with a pressure
event or jaw-sized motion (x1.8-4.5); maybe = either one alone; gross movement is flagged separately.
Prints, for every calibration step, how many of its "do" phases were caught and how much of its
rest time raised a false alarm.
"""

import sys

import numpy as np

import analyze_calibration as ac

FRAME_MS = 100
SOUND_EXCESS_DB = 8.0      # 250-2000 Hz above its quiet floor
VOICE_LOW_DB = 9.0         # <250 Hz excess that marks voice / cough (speech p25 16, cough 13, grinding p75 <=7)
SCRATCH_MARGIN_DB = 5.0    # 250-2000 Hz rose less than this above 2-4 kHz -> broadband: scratch / rub
PRESSURE_RISE = 40.0       # counts above the 5 s median
PRESSURE_OSC = 25.0        # counts, 1 s standard deviation
MOTION_JAW = (1.8, 4.5)    # motion spread over its floor: jaw-sized (grinding x2.5-4, touching the cheek x5.7)
MOTION_GROSS = 8.0         # and body-sized
MERGE_MS = 1500
SOUND_MIN_FRAMES = 6       # grinding-like frames in an episode (0.6 s of sound)...
SOUND_MIN_MS = 1500        # ...spread over at least this long
VOICE_VETO = 0.4           # share of loud frames that are voice-like -> speech / cough (speech ~80%, grinding <25%)
SCRATCH_VETO = 0.4         # share that are scratch-like -> touching near the patch


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


def main():
    flushes, calib = ac.load(sys.argv[1])
    s = ac.build_series(flushes)
    runs = ac.split_steps(calib)
    mic_lag, _, beep_times, _ = ac.detect_beeps(s, runs)

    t = s['frame_t'] - mic_lag             # sound frames moved onto the sensors' clock
    bp = s['band_power']
    order = np.argsort(t)
    t, bp = t[order], bp[order]
    beep_mask = np.zeros(len(t), bool)
    for b in beep_times - mic_lag:
        beep_mask |= (t >= b + ac.BEEP_EXCLUDE_MS[0]) & (t <= b + ac.BEEP_EXCLUDE_MS[1])

    to_db = lambda x: 10 * np.log10(x + 1e-9)
    brux = to_db(bp[:, 1:8].sum(axis=1))
    low = to_db(bp[:, 0])
    high = to_db(bp[:, [8, 9, 12, 13, 14]].sum(axis=1))
    for arr in (brux, low, high):
        arr[beep_mask] = np.nan
    brux_x = brux - rolling_floor(brux, t, 30000, 20)
    low_x = low - rolling_floor(low, t, 30000, 20)
    high_x = high - rolling_floor(high, t, 30000, 20)

    fsm = resample(s['fsm_t'], s['fsm'], t, np.mean, 60)
    fsm_dev = fsm - rolling_floor(fsm, t, 5000, 50)
    fsm_osc = resample(s['fsm_t'], s['fsm'], t, np.std, 500)
    acc_sd = resample(s['acc_t'], s['acc'], t, np.std, 250)
    acc_ratio = acc_sd / np.maximum(rolling_floor(acc_sd, t, 30000, 20), 1.0)

    loud = brux_x > SOUND_EXCESS_DB
    voice = loud & (low_x > VOICE_LOW_DB)
    scratch = loud & ((brux_x - high_x) < SCRATCH_MARGIN_DB) & ~voice
    sound_hit = loud & ~voice & ~scratch
    press_hit = (fsm_dev > PRESSURE_RISE) | (fsm_osc > PRESSURE_OSC)
    gross = acc_ratio > MOTION_GROSS
    jaw = (acc_ratio > MOTION_JAW[0]) & (acc_ratio <= MOTION_JAW[1])

    # Episodes: stretches of sound or pressure hits, merged across short gaps.
    hit = np.nan_to_num(sound_hit.astype(float)) + np.nan_to_num(press_hit.astype(float)) > 0
    episodes, start, last = [], None, None
    for i in np.where(hit)[0]:
        if start is None:
            start = last = i
        elif t[i] - t[last] <= MERGE_MS:
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
        loud = max(n_sound + n_voice + n_scratch, 1)
        duration = t[b] - t[a] + FRAME_MS
        is_gross = np.nanmean(gross[sl]) > 0.3
        # Grinding goes on for seconds; a swallow is one short sound. Speech and coughs keep some
        # voice-like frames even where single frames look like grinding -- veto the whole episode.
        sound_ok = n_sound >= SOUND_MIN_FRAMES and duration >= SOUND_MIN_MS
        if n_voice / loud >= VOICE_VETO or n_scratch / loud >= SCRATCH_VETO:
            sound_ok = False
        press_ok = n_press >= 3
        if not sound_ok and not press_ok:
            continue
        if is_gross:
            level = 'movement'
        elif sound_ok and (press_ok or np.nanmean(jaw[sl]) > 0.3):
            level = 'likely'
        else:
            level = 'maybe'
        graded.append({'t0': t[a], 't1': t[b], 'level': level, 'sound': n_sound, 'press': n_press,
                       'voice': n_voice, 'scratch': n_scratch})

    offset = s['offset']
    print(f'{"step":18} {"do phases caught (maybe / likely)":>34} {"false alarms in rest":>22}   movement')
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
        caught = f'{caught_maybe + caught_likely}/{len(dos)}  ({caught_maybe} / {caught_likely})'
        alarm = f'{alarm_ms / max(rest_ms, 1) * 100:.0f}% of {rest_ms / 1000:.0f} s'
        print(f'{run["step"]:18} {caught:>34} {alarm:>22}   {moves}')


if __name__ == '__main__':
    main()
