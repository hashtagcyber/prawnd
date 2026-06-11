import { ulid } from 'ulid';
import path from 'node:path';
import fs from 'node:fs';
import crypto from 'node:crypto';
import { parseWavHeader } from '../wav.js';
import { denoise } from '../denoise.js';

const rawParser = (req, payload, done) => done(null, payload);

export default async function uploadRoute(app) {
  app.addContentTypeParser('audio/wav', rawParser);
  app.addContentTypeParser('application/octet-stream', rawParser);

  app.post('/upload', async (req, reply) => {
    const stream = req.body;
    if (!stream || typeof stream.on !== 'function') {
      return reply.code(400).send({ error: 'no_body' });
    }

    const id = ulid();
    const deviceId = (req.headers['x-device-id'] || 'unknown')
      .toString()
      .replace(/[^A-Za-z0-9_-]/g, '_')
      .slice(0, 64) || 'unknown';
    const now = new Date();
    const y = now.getUTCFullYear();
    const m = String(now.getUTCMonth() + 1).padStart(2, '0');
    const relDir = path.posix.join('audio', deviceId, String(y), m);
    const absDir = path.join(app.dataDir, relDir);
    fs.mkdirSync(absDir, { recursive: true });
    const relPath = path.posix.join(relDir, `${id}.wav`);
    const absPath = path.join(app.dataDir, relPath);
    const tmpPath = absPath + '.part';

    const out = fs.createWriteStream(tmpPath);
    const hash = crypto.createHash('sha256');
    const headerChunks = [];
    let headerBytes = 0;
    let totalBytes = 0;

    try {
      for await (const chunk of stream) {
        totalBytes += chunk.length;
        hash.update(chunk);
        if (headerBytes < 44) {
          const want = 44 - headerBytes;
          headerChunks.push(chunk.subarray(0, Math.min(chunk.length, want)));
          headerBytes += Math.min(chunk.length, want);
        }
        if (!out.write(chunk)) {
          await new Promise((res) => out.once('drain', res));
        }
      }
      await new Promise((res, rej) =>
        out.end((err) => (err ? rej(err) : res()))
      );
    } catch (e) {
      try { fs.unlinkSync(tmpPath); } catch {}
      req.log.error({ err: e }, 'upload stream failed');
      return reply.code(500).send({ error: 'upload_failed' });
    }

    if (headerBytes < 44) {
      try { fs.unlinkSync(tmpPath); } catch {}
      return reply.code(400).send({ error: 'too_short' });
    }

    let header;
    try {
      header = parseWavHeader(Buffer.concat(headerChunks, 44));
    } catch (e) {
      try { fs.unlinkSync(tmpPath); } catch {}
      return reply.code(400).send({ error: 'bad_wav', detail: e.message });
    }

    fs.renameSync(tmpPath, absPath);
    const sha256 = hash.digest('hex');
    const dataBytes = Math.max(0, totalBytes - 44);
    const duration_sec = header.byte_rate > 0 ? dataBytes / header.byte_rate : 0;
    const client_ts = Number(req.headers['x-client-timestamp']) || null;

    // Run denoise inline so the cleaned file exists by the time the client
    // (or UI) hits /recordings/:id/file?variant=cleaned. For a few-second WAV
    // this takes ~50-150 ms — well under the upload latency budget.
    const cleanedRelPath = relPath.replace(/\.wav$/i, '.cleaned.wav');
    const cleanedAbsPath = path.join(app.dataDir, cleanedRelPath);
    let cleanedStored = null;
    try {
      await denoise(absPath, cleanedAbsPath, req.log);
      cleanedStored = cleanedRelPath;
    } catch (e) {
      req.log.warn({ err: e.message }, 'denoise failed; serving original only');
    }

    app.db.prepare(`
      INSERT INTO recordings (id, device_id, received_at, client_ts, path,
                              size_bytes, duration_sec, sample_rate, channels,
                              sha256, cleaned_path)
      VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    `).run(
      id, deviceId, Date.now(), client_ts, relPath,
      totalBytes, duration_sec, header.sample_rate, header.channels, sha256,
      cleanedStored
    );

    return reply.code(200).send({
      id,
      device_id: deviceId,
      size_bytes: totalBytes,
      duration_sec,
      sample_rate: header.sample_rate,
      channels: header.channels,
      sha256,
      cleaned: cleanedStored != null,
    });
  });
}
