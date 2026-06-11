export function requireApiKey(req, reply, done) {
  const expected = process.env.PRAWND_API_KEY;
  if (!expected) {
    req.log.error('PRAWND_API_KEY env var not set');
    reply.code(500).send({ error: 'server_misconfigured' });
    return;
  }
  const auth = req.headers.authorization || '';
  const bearer = auth.match(/^Bearer\s+(.+)$/i)?.[1];
  const got = bearer || req.headers['x-api-key'] || req.query?.api_key;
  if (got !== expected) {
    reply.code(401).send({ error: 'unauthorized' });
    return;
  }
  done();
}
