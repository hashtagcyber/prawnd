export default async function healthRoute(app) {
  app.get('/healthz', async () => ({ ok: true }));
}
