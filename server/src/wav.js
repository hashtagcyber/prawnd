export function parseWavHeader(buf) {
  if (buf.length < 44) throw new Error('header too small');
  if (buf.toString('ascii', 0, 4) !== 'RIFF') throw new Error('not RIFF');
  if (buf.toString('ascii', 8, 12) !== 'WAVE') throw new Error('not WAVE');
  if (buf.toString('ascii', 12, 16) !== 'fmt ') throw new Error('missing fmt chunk');
  const channels = buf.readUInt16LE(22);
  const sample_rate = buf.readUInt32LE(24);
  const byte_rate = buf.readUInt32LE(28);
  const bits_per_sample = buf.readUInt16LE(34);
  return { channels, sample_rate, byte_rate, bits_per_sample };
}
