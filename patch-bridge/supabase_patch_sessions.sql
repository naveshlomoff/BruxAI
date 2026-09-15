-- ============================================================
-- BroxMon patch sessions — cloud storage (run once in Supabase SQL editor)
--
-- What the Patch tab saves when a recording is stopped:
--   * one row in patch_sessions: times, device, detected events, the mic level series
--     (bridge micRms, ~2 readings per second) and per-channel sample counts;
--   * the raw samples (mic / acc / fsm arrays) as one gzipped JSON file in the private
--     Storage bucket "patch-raw", at {user_id}/{session_id}.json.gz (raw_path below).
-- Same ownership model as nights/events (see supabase_rls_fix.sql): every row and file
-- belongs to the signed-in (including anonymous) user, and only that user can read it.
-- Safe to re-run.
-- ============================================================

-- ── TABLE ────────────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS patch_sessions (
  id            uuid PRIMARY KEY DEFAULT gen_random_uuid(),
  user_id       uuid NOT NULL DEFAULT auth.uid() REFERENCES auth.users(id) ON DELETE CASCADE,
  session_id    bigint NOT NULL,              -- recording start, ms since epoch (the app's local key)
  started_at    timestamptz NOT NULL,
  ended_at      timestamptz NOT NULL,
  duration_sec  integer NOT NULL,
  device_name   text,
  event_count   integer NOT NULL DEFAULT 0,
  events        jsonb NOT NULL DEFAULT '[]',  -- [{start, end, channels, confidence}] (ms since epoch)
  mic_level     jsonb,                        -- [[ms since session start, rms], ...]
  sample_counts jsonb,                        -- {"mic": n, "acc": n, "fsm": n}
  raw_path      text,                         -- object path in bucket patch-raw
  created_at    timestamptz NOT NULL DEFAULT now(),
  CONSTRAINT patch_sessions_user_session_unique UNIQUE (user_id, session_id)
);

ALTER TABLE patch_sessions ENABLE ROW LEVEL SECURITY;

DROP POLICY IF EXISTS "patch_sessions_insert_own" ON patch_sessions;
DROP POLICY IF EXISTS "patch_sessions_select_own" ON patch_sessions;
DROP POLICY IF EXISTS "patch_sessions_update_own" ON patch_sessions;
DROP POLICY IF EXISTS "patch_sessions_delete_own" ON patch_sessions;

CREATE POLICY "patch_sessions_insert_own"
  ON patch_sessions FOR INSERT TO authenticated
  WITH CHECK (user_id = auth.uid());

CREATE POLICY "patch_sessions_select_own"
  ON patch_sessions FOR SELECT TO authenticated
  USING (user_id = auth.uid());

CREATE POLICY "patch_sessions_update_own"
  ON patch_sessions FOR UPDATE TO authenticated
  USING (user_id = auth.uid())
  WITH CHECK (user_id = auth.uid());

CREATE POLICY "patch_sessions_delete_own"
  ON patch_sessions FOR DELETE TO authenticated
  USING (user_id = auth.uid());

-- ── STORAGE BUCKET (private) ─────────────────────────────────
INSERT INTO storage.buckets (id, name, public)
VALUES ('patch-raw', 'patch-raw', false)
ON CONFLICT (id) DO NOTHING;

-- Files live under a folder named after the owner's user id.
DROP POLICY IF EXISTS "patch_raw_insert_own" ON storage.objects;
DROP POLICY IF EXISTS "patch_raw_select_own" ON storage.objects;
DROP POLICY IF EXISTS "patch_raw_update_own" ON storage.objects;
DROP POLICY IF EXISTS "patch_raw_delete_own" ON storage.objects;

CREATE POLICY "patch_raw_insert_own"
  ON storage.objects FOR INSERT TO authenticated
  WITH CHECK (bucket_id = 'patch-raw' AND (storage.foldername(name))[1] = auth.uid()::text);

CREATE POLICY "patch_raw_select_own"
  ON storage.objects FOR SELECT TO authenticated
  USING (bucket_id = 'patch-raw' AND (storage.foldername(name))[1] = auth.uid()::text);

-- upsert (re-saving the same session) needs UPDATE as well as INSERT
CREATE POLICY "patch_raw_update_own"
  ON storage.objects FOR UPDATE TO authenticated
  USING (bucket_id = 'patch-raw' AND (storage.foldername(name))[1] = auth.uid()::text)
  WITH CHECK (bucket_id = 'patch-raw' AND (storage.foldername(name))[1] = auth.uid()::text);

CREATE POLICY "patch_raw_delete_own"
  ON storage.objects FOR DELETE TO authenticated
  USING (bucket_id = 'patch-raw' AND (storage.foldername(name))[1] = auth.uid()::text);
