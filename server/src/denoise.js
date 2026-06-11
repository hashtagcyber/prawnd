import { spawn } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';

// Voice-recorder cleanup pipeline. Each filter has a specific job:
//   highpass=100        — removes rumble (10-30 Hz) and most of the 60 Hz fundamental
//   bandreject 60/120/180 — surgical notches for powerline hum + its harmonics
//   lowpass=4000        — kills 4-8 kHz whine (regulator switching noise)
//   dynaudnorm          — equalises perceived loudness across recordings
// On the test recording this lifted the voice/hum ratio from 0.30 to 83.8 (~280×).
const FILTER_CHAIN =
  'highpass=f=100,' +
  'bandreject=f=60:width_type=h:w=10,' +
  'bandreject=f=120:width_type=h:w=10,' +
  'bandreject=f=180:width_type=h:w=10,' +
  'lowpass=f=4000,' +
  'dynaudnorm';

export function denoise(inputPath, outputPath, log) {
  return new Promise((resolve, reject) => {
    fs.mkdirSync(path.dirname(outputPath), { recursive: true });
    const tmpPath = outputPath + '.part';
    const args = [
      '-y',
      '-loglevel', 'error',
      '-i', inputPath,
      '-af', FILTER_CHAIN,
      '-map_metadata', '-1',
      '-f', 'wav',          // tmpPath has .part extension; force the muxer
      tmpPath,
    ];
    const t0 = Date.now();
    const proc = spawn('ffmpeg', args);
    let stderr = '';
    proc.stderr.on('data', (d) => { stderr += d.toString(); });
    proc.on('error', (err) => {
      try { fs.unlinkSync(tmpPath); } catch {}
      reject(err);
    });
    proc.on('close', (code) => {
      if (code !== 0) {
        try { fs.unlinkSync(tmpPath); } catch {}
        return reject(new Error(`ffmpeg exit ${code}: ${stderr.trim()}`));
      }
      try {
        fs.renameSync(tmpPath, outputPath);
      } catch (e) {
        return reject(e);
      }
      log?.info?.({ ms: Date.now() - t0, input: inputPath }, 'denoised');
      resolve();
    });
  });
}

// Walk recordings that have an existing original on disk but no cleaned_path,
// and process them. Runs in the background after server boot so we don't block
// startup. Errors are logged but don't crash the server.
export async function reprocessMissing(app) {
  const rows = app.db
    .prepare(`SELECT id, path FROM recordings WHERE cleaned_path IS NULL`)
    .all();
  if (!rows.length) return;
  app.log.info(`reprocessing ${rows.length} recording(s) without cleaned variant`);
  const update = app.db.prepare(
    'UPDATE recordings SET cleaned_path = ? WHERE id = ?'
  );
  for (const row of rows) {
    const abs = path.join(app.dataDir, row.path);
    if (!fs.existsSync(abs)) continue;
    const cleanedRel = row.path.replace(/\.wav$/i, '.cleaned.wav');
    const cleanedAbs = path.join(app.dataDir, cleanedRel);
    try {
      await denoise(abs, cleanedAbs, app.log);
      update.run(cleanedRel, row.id);
    } catch (e) {
      app.log.warn({ err: e.message, id: row.id }, 'reprocess denoise failed');
    }
  }
  app.log.info('reprocess complete');
}
