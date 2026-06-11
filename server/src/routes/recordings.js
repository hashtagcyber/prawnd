import path from 'node:path';
import fs from 'node:fs';

export default async function recordingsRoute(app) {
  app.get('/recordings', async (req) => {
    const limit = Math.min(Math.max(Number(req.query.limit) || 50, 1), 500);
    const cursor = req.query.cursor;
    const deviceId = req.query.device_id;

    let sql = `SELECT id, device_id, received_at, client_ts, path, cleaned_path,
                      size_bytes, duration_sec, sample_rate, channels, sha256
               FROM recordings WHERE 1=1`;
    const params = [];
    if (deviceId) {
      sql += ' AND device_id = ?';
      params.push(deviceId);
    }
    if (cursor) {
      const [cRecv, cId] = String(cursor).split('_');
      sql += ' AND (received_at < ? OR (received_at = ? AND id < ?))';
      params.push(Number(cRecv), Number(cRecv), cId);
    }
    sql += ' ORDER BY received_at DESC, id DESC LIMIT ?';
    params.push(limit);

    const rows = app.db.prepare(sql).all(...params);
    const next_cursor =
      rows.length === limit
        ? `${rows[rows.length - 1].received_at}_${rows[rows.length - 1].id}`
        : null;
    return { items: rows, next_cursor };
  });

  app.get('/recordings/:id', async (req, reply) => {
    const row = app.db.prepare('SELECT * FROM recordings WHERE id = ?').get(req.params.id);
    if (!row) return reply.code(404).send({ error: 'not_found' });
    return row;
  });

  app.get('/recordings/:id/file', async (req, reply) => {
    const row = app.db
      .prepare('SELECT path, cleaned_path, size_bytes FROM recordings WHERE id = ?')
      .get(req.params.id);
    if (!row) return reply.code(404).send({ error: 'not_found' });
    // ?variant=cleaned serves the denoised file when available; default = original
    const variant = req.query.variant === 'cleaned' ? 'cleaned' : 'original';
    let rel = row.path;
    if (variant === 'cleaned' && row.cleaned_path) rel = row.cleaned_path;
    const abs = path.join(app.dataDir, rel);
    if (!fs.existsSync(abs)) return reply.code(404).send({ error: 'file_missing' });
    reply.header('Content-Type', 'audio/wav');
    if (variant === 'original') reply.header('Content-Length', row.size_bytes);
    return reply.send(fs.createReadStream(abs));
  });
}
