# BruxTrack Roadmap — Phase-by-Phase

> Condensed reference for Claude Code. Full disclosure document lives outside the repo (patent-protected).

## Goal

Move BruxTrack from a passive frequency-threshold detector to a learning system that computes the patent-protected BSI severity score, adapts to each user, and eventually classifies events with on-device ML.

---

## Phase 0 — Supabase Connection (THIS WEEK) 🟡

**Goal:** Every night and every labeled event saved to Supabase automatically. Audio clips uploaded to Storage. Zero data loss from this point forward.

**Tasks:**
- [x] Create Supabase project (`bruxtrack` / `ukoswihzqpztfypqhdnc`)
- [x] Create tables: `nights`, `events`, `population_stats`
- [x] Create Storage bucket: `audio-clips`
- [x] Add Supabase JS SDK via CDN
- [ ] **Verify RLS policies on all tables** (currently the repo is public)
- [ ] Implement `syncNightToCloud()` — already exists in `index.html`, needs hardening
- [ ] Implement `uploadClip()` — already exists, needs retry-on-failure
- [ ] Implement `loadLastNightFromCloud()` — already exists, needs fallback paths
- [ ] Add visible sync status indicator (✓ synced / ⚠ pending / ✗ failed)
- [ ] Test end-to-end: clear IndexedDB, reopen app, verify data loads from cloud

**Definition of done:** Clearing browser storage does not lose a single night.

---

## Phase 1 — Data Collection (2–3 weeks) ⬜

**Goal:** Accumulate 5–10 nights of fully labeled data. Every candidate event listened to and labeled (bruxism / not bruxism / unsure).

**Targets:**
- 30+ confirmed bruxism events
- 30+ confirmed non-bruxism events
- Notes on false positive rate per night

**App tasks:**
- [ ] CSV export of all labeled events
- [ ] False-positive-rate display in settings

---

## Phase 2 — BSI Formula (1 week) 🟡

**Goal:** Patent-protected BSI score computed after every night.

**Status note (2026-06-08):** Implemented as a **client-side prototype** first (in `index.html`,
alongside `syncNightToCloud`) rather than starting with a Deno Edge Function — lets us validate
the formula and population-bootstrap behavior against real labeled data before committing to a
server-side schema/migration. Porting to an Edge Function remains the follow-up step once the
prototype is validated against a few nights of real scores.

**Tasks:**
- [ ] Set up Deno Edge Function runtime *(deferred — prototype runs client-side for now)*
- [x] Implement `compute_bsi()` — Fp, Dp, Ap, C, weighted sum *(client-side: `computeBSI()`)*
- [x] Seed `population_stats` from initial nights *(client-side: `computeBsiPopulationStats()` —
      computed on-demand from the user's own labeled nights in IndexedDB; bootstraps once ≥5
      labeled nights exist, per the patent's bootstrap-from-own-history approach. Not yet
      persisted to the Supabase `population_stats` table — that's part of the Edge Function port.)*
- [ ] Trigger BSI computation on night sync *(currently computed on-demand when the report/history
      screens render — `computeAndStoreBSI()`; not yet triggered from `syncNightToCloud` itself,
      and not yet synced to the cloud `nights.bsi_score` / `ebi_cumulative` columns — needs schema
      verification first per CLAUDE.md §10 "stop and ask before touching the Supabase schema")*
- [x] Display BSI in morning report *(added alongside the existing raw-activity-score card —
      did not replace it, so the existing UI/labeling flow stays intact)*
- [x] Severity bands: None/minimal (0–15) / Mild (16–35) / Moderate (36–60) / Severe (61–79) /
      Very severe (80–100) — exact cut points per P2 §4.3 (`bsiSeverityBand()`)
- [x] Compute and store EBI = Σ BSI(n) / N *(client-side, local-only: `computeEBI()` +
      `renderEbiCard()` on the History screen)*
- [x] Compute and store EBI² = Σ BSI(n)² / N *(same — `computeEBI()` returns both)*

---

## Phase 3 — Adaptive Threshold (1 week) ⬜

**Goal:** Per-user, per-environment threshold replaces the static `sensitivity=45`.

**Algorithm:**
- Compute personal_baseline = 90th percentile of non-bruxism event amplitudes (over 5+ labeled nights)
- New threshold = personal_baseline × 1.15
- Threshold updated nightly, synced to app on open

---

## Phase 4 — ML Classifier (15+ labeled nights) ⬜

**Goal:** TensorFlow.js binary classifier replaces threshold detector. Runs in browser, <1ms per event.

**Features (9):** duration, peakHz, amplitude, dB, spectral centroid, spectral spread, spectral flatness, periodicity score, time-of-night.

**Pipeline:** Export labeled events as CSV → train sklearn model in Python → convert to TF.js → embed in app. Retrain trigger: every 50 new labels.

---

## Phase 5 — Population Registry (multi-user) ⬜

**Goal:** Opt-in anonymized data → true population percentile normalization for BSI. Supports future clinical validation. Maps to Patent P7.

---

## Patent Alignment Quick Reference

| Phase | Patent Family | Note |
| --- | --- | --- |
| 0 | P2 §4.5 | Data persistence prerequisite for normalization |
| 1 | P2 §4.1 | C ratio = confirmed/total — core BSI input |
| 2 | P2 claims 1–14 | BSI + EBI direct implementation |
| 3 | P1 | Personalized acoustic detection |
| 4 | P4 | Acoustic-only embodiment of sensor fusion |
| 5 | P7 | Population registry |
