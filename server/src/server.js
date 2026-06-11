import Fastify from 'fastify';
import path from 'node:path';
import fs from 'node:fs';
import { initDb } from './db.js';
import { requireApiKey } from './auth.js';
import uploadRoute from './routes/upload.js';
import recordingsRoute from './routes/recordings.js';
import healthRoute from './routes/health.js';
import uiRoute from './routes/ui.js';
import { reprocessMissing } from './denoise.js';

const PORT = Number(process.env.PORT || 8080);
const HOST = process.env.HOST || '0.0.0.0';
const DATA_DIR = path.resolve(process.env.DATA_DIR || './data');

fs.mkdirSync(path.join(DATA_DIR, 'audio'), { recursive: true });
const db = initDb(path.join(DATA_DIR, 'prawnd.sqlite'));

const app = Fastify({
  logger: { level: process.env.LOG_LEVEL || 'info' },
  bodyLimit: 200 * 1024 * 1024,
});

app.decorate('db', db);
app.decorate('dataDir', DATA_DIR);

await app.register(healthRoute);
await app.register(uiRoute);
await app.register(async (instance) => {
  instance.addHook('onRequest', requireApiKey);
  await instance.register(uploadRoute);
  await instance.register(recordingsRoute);
});

try {
  await app.listen({ port: PORT, host: HOST });
  app.log.info(`prawnd listening on ${HOST}:${PORT}, data dir ${DATA_DIR}`);
  // Backfill cleaned variants for any pre-existing recordings — runs in the
  // background; errors are logged, server keeps serving.
  reprocessMissing(app).catch((err) =>
    app.log.warn({ err: err.message }, 'reprocessMissing failed')
  );
} catch (err) {
  app.log.error(err);
  process.exit(1);
}
