import Database from 'better-sqlite3';

export function initDb(filePath) {
  const db = new Database(filePath);
  db.pragma('journal_mode = WAL');
  db.pragma('synchronous = NORMAL');
  db.exec(`
    CREATE TABLE IF NOT EXISTS recordings (
      id            TEXT PRIMARY KEY,
      device_id     TEXT NOT NULL,
      received_at   INTEGER NOT NULL,
      client_ts     INTEGER,
      path          TEXT NOT NULL,
      size_bytes    INTEGER NOT NULL,
      duration_sec  REAL NOT NULL,
      sample_rate   INTEGER NOT NULL,
      channels      INTEGER NOT NULL,
      sha256        TEXT NOT NULL
    );
    CREATE INDEX IF NOT EXISTS idx_recordings_device_received
      ON recordings(device_id, received_at DESC);
    CREATE INDEX IF NOT EXISTS idx_recordings_received
      ON recordings(received_at DESC);
  `);
  // Additive migrations — wrap in try/catch because SQLite has no IF NOT EXISTS for columns.
  try { db.exec('ALTER TABLE recordings ADD COLUMN cleaned_path TEXT'); } catch {}
  return db;
}
